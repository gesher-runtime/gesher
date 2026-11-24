/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.app.ActivityThread;
import android.content.AttributionSourceState;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.hardware.ICameraService;
import android.hardware.camera2.ICameraDeviceCallbacks;
import android.hardware.camera2.ICameraDeviceUser;
import android.os.Binder;
import android.os.Process;
import android.os.RemoteException;
import android.os.ServiceSpecificException;
import android.os.UserHandle;
import android.util.Size;
import com.gesher.server.CameraService;
import com.gesher.server.LegacyCamera;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Runs both complete production admission paths. Only package/permission answers and
 * camera enumeration are fixture boundaries; no hardware, capture, or remote Binder
 * observation is claimed. The selected camera never opens a native capture session.
 */
public final class CameraPermissionContractTest {
    private static final String OWNER = "com.gesher.camera.permission.owner";
    private static final String SIBLING = "com.gesher.camera.permission.sibling";
    private static final String FOREIGN = "com.gesher.camera.permission.foreign";
    private static int checks;

    private static void check(boolean value, String message) {
        checks++;
        if (!value) throw new AssertionError(message);
    }

    private static Field field(Class<?> type, String name) throws Exception {
        Field field = type.getDeclaredField(name);
        field.setAccessible(true);
        return field;
    }

    private static Object allocate(Class<?> type) throws Exception {
        Class<?> unsafeClass = Class.forName("sun.misc.Unsafe");
        for (Field singleton : unsafeClass.getDeclaredFields()) {
            if (singleton.getType() != unsafeClass || !Modifier.isStatic(singleton.getModifiers())) continue;
            singleton.setAccessible(true);
            return unsafeClass.getMethod("allocateInstance", Class.class)
                    .invoke(singleton.get(null), type);
        }
        throw new AssertionError("No Unsafe singleton for isolated fixture allocation");
    }

    /** A live permission-service boundary, deliberately independent of manifest requests. */
    private static final class Packages extends IPackageManager.Default {
        final Map<String, Integer> owners = new HashMap<>();
        boolean granted, declared = true;
        boolean failLookup, failPermission;
        int permissionReads;

        Packages() {
            owners.put(OWNER, Process.myUid());
            owners.put(SIBLING, Process.myUid());
            owners.put(FOREIGN, Process.myUid() + 1);
        }

        private void identity(int userId) {
            check(userId == UserHandle.getUserId(Process.myUid()), "permission user scope");
            check(Binder.getCallingUid() == Process.myUid(), "inherited Binder UID leaked into local query");
            check(Binder.getCallingPid() == Process.myPid(), "inherited Binder PID leaked into local query");
        }

        @Override public int getPackageUid(String name, long flags, int userId) throws RemoteException {
            identity(userId);
            if (failLookup) throw new RemoteException("fixture package service unavailable");
            return owners.getOrDefault(name, -1);
        }

        @Override public int checkUidPermission(String permission, int uid)
                throws RemoteException {
            identity(UserHandle.getUserId(uid));
            check(uid == Process.myUid(), "permission query used claimed rather than actual UID");
            check(Manifest.permission.CAMERA.equals(permission), "wrong runtime permission queried");
            permissionReads++;
            if (failPermission) throw new RemoteException("fixture permission service unavailable");
            return granted
                    ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
        }

        // The old implementation asks only this question. Keeping it independent
        // makes the unchanged fixture fail on the real pre-fix CameraService.
        @Override public PackageInfo getPackageInfo(String name, long flags, int userId) {
            if (!owners.containsKey(name)) return null;
            PackageInfo info = new PackageInfo();
            info.packageName = name;
            info.requestedPermissions = declared ? new String[] {Manifest.permission.CAMERA}
                    : new String[0];
            return info;
        }
    }

    private static AttributionSourceState source(String name) {
        AttributionSourceState source = new AttributionSourceState();
        source.uid = Process.myUid();
        source.pid = Process.myPid();
        source.packageName = name;
        return source;
    }

    private static ICameraDeviceUser camera2(CameraService service, AttributionSourceState source) {
        return service.connectDevice(new ICameraDeviceCallbacks.Default(), "0", 0, 36,
                null, source, 0, false);
    }

    private static void denied(CameraService service, AttributionSourceState source, String reason)
            throws Exception {
        Object previous = ((Map<?, ?>) field(CameraService.class, "mOpen").get(service)).get("0");
        try {
            ICameraDeviceUser unexpected = camera2(service, source);
            if (unexpected != null) unexpected.disconnect();
            throw new AssertionError(reason + ": Camera2 admitted the caller");
        } catch (ServiceSpecificException expected) {
            check(expected.errorCode == ICameraService.ERROR_PERMISSION_DENIED,
                    reason + ": wrong Camera2 error");
        }
        check(((Map<?, ?>) field(CameraService.class, "mOpen").get(service)).get("0") == previous,
                reason + ": denial changed camera ownership");
    }

    private static void both(CameraService service, ApplicationInfo application, String name,
            boolean allowed, String reason) throws Exception {
        application.packageName = name;
        if (allowed) {
            ICameraDeviceUser device = camera2(service, source(name));
            check(device != null, reason + ": Camera2 did not open");
            device.disconnect();
        } else {
            denied(service, source(name), reason);
        }
        // A null Camera is only an isolated session-map key, avoiding a native Camera
        // constructor. setup/release themselves are the complete production methods.
        int result = LegacyCamera.setup(null, null, 0);
        check(result == (allowed ? 0 : -13), reason + ": wrong legacy result " + result);
        Map<?, ?> sessions = (Map<?, ?>) field(LegacyCamera.class, "sSessions").get(null);
        check(sessions.containsKey(null) == allowed, reason + ": legacy denial allocated a session");
        LegacyCamera.release(null);
        check(sessions.isEmpty(), reason + ": legacy session retained after release");
        check(((Map<?, ?>) field(CameraService.class, "mOpen").get(service)).isEmpty(),
                reason + ": Camera2 owner retained after disconnect");
    }

    public static void contracts() throws Exception {
        Field manager = field(ActivityThread.class, "sPackageManager");
        Field current = field(ActivityThread.class, "sCurrentActivityThread");
        Object oldManager = manager.get(null), oldThread = current.get(null);
        CameraService service = CameraService.get();
        Field devices = field(CameraService.class, "mDevices");
        Object oldDevices = devices.get(service);
        Packages packages = new Packages();
        Object thread = allocate(ActivityThread.class);
        Class<?> boundClass = Class.forName("android.app.ActivityThread$AppBindData");
        Object bound = allocate(boundClass);
        ApplicationInfo application = new ApplicationInfo();
        application.packageName = OWNER;
        application.uid = Process.myUid();
        field(boundClass, "appInfo").set(bound, application);
        field(ActivityThread.class, "mBoundApplication").set(thread, bound);
        Class<?> deviceClass = Class.forName("com.gesher.server.WindowsCamera$Device");
        Constructor<?> constructor = deviceClass.getDeclaredConstructor(String.class, String.class,
                List.class, boolean.class);
        constructor.setAccessible(true);
        Object device = constructor.newInstance("isolated-admission-fixture", "admission fixture",
                List.of(new Size(320, 240)), false);
        long initialIdentity = Binder.clearCallingIdentity();
        try {
            manager.set(null, packages);
            current.set(null, thread);
            devices.set(service, List.of(device));
            both(service, application, OWNER, false, "declared CAMERA is initially denied");
            check(packages.permissionReads >= 2, "both APIs must query the runtime grant");
            packages.granted = true;
            both(service, application, OWNER, true, "explicit runtime grant");
            ICameraDeviceUser active = camera2(service, source(OWNER));
            packages.granted = false;
            denied(service, source(OWNER), "revoked caller cannot evict an existing owner");
            active.disconnect();
            both(service, application, OWNER, false, "revocation is visible on the next open");
            packages.declared = false;
            both(service, application, OWNER, false, "undeclared CAMERA is denied");
            packages.granted = true;
            both(service, application, SIBLING, true, "same-UID sibling uses effective shared grant");
            packages.declared = true;
            both(service, application, FOREIGN, false, "another UID cannot lend its grant");
            both(service, application, "missing.package", false, "missing package");
            both(service, application, null, false, "missing package attribution");
            both(service, application, "", false, "empty package attribution");
            denied(service, null, "missing Camera2 attribution");
            AttributionSourceState forged = source(OWNER);
            forged.uid++;
            denied(service, forged, "forged attribution UID");
            forged = source(OWNER);
            forged.pid++;
            denied(service, forged, "forged attribution PID");
            forged = source(OWNER);
            forged.uid = -2;
            denied(service, forged, "invalid negative attribution UID");
            AttributionSourceState implicit = source(OWNER);
            implicit.uid = ICameraService.USE_CALLING_UID;
            implicit.pid = ICameraService.USE_CALLING_PID;
            camera2(service, implicit).disconnect();
            checks++;
            packages.failLookup = true;
            both(service, application, OWNER, false, "package-service failure fails closed");
            packages.failLookup = false;
            packages.failPermission = true;
            both(service, application, OWNER, false, "permission-service failure fails closed");
            packages.failPermission = false;
            packages.owners.put(OWNER, Process.myUid() + 1);
            both(service, application, OWNER, false, "reassigned package UID is not cached");
            packages.owners.put(OWNER, Process.myUid());
            long foreign = ((long) (Process.myUid() + 37) << 32) | (Process.myPid() + 1);
            Binder.restoreCallingIdentity(foreign);
            both(service, application, OWNER, true, "open from an incoming Binder callback");
            check(Binder.getCallingUid() == Process.myUid() + 37
                    && Binder.getCallingPid() == Process.myPid() + 1,
                    "camera query did not restore the incoming Binder identity");
            Binder.clearCallingIdentity();
            manager.set(null, new Packages());
            both(service, application, OWNER, false, "replacement permission service is queried");
        } finally {
            Binder.restoreCallingIdentity(initialIdentity);
            LegacyCamera.release(null);
            manager.set(null, oldManager);
            current.set(null, oldThread);
            devices.set(service, oldDevices);
        }
        System.out.println("CameraPermissionContractTest: both admission paths, live grants and UID attribution passed ("
                + checks + " checks; isolated package and device boundaries)");
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        contracts();
    }
}
