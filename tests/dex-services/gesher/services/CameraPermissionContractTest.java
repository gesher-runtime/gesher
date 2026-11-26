/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.app.ActivityThread;
import android.app.AppOpsManager;
import android.app.SyncNotedAppOp;
import android.content.AttributionSourceState;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.hardware.Camera;
import android.hardware.ICameraService;
import android.hardware.camera2.ICameraDeviceCallbacks;
import android.hardware.camera2.ICameraDeviceUser;
import android.hardware.camera2.impl.CaptureResultExtras;
import android.os.Binder;
import android.os.IBinder;
import android.os.Process;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.ServiceSpecificException;
import android.os.UserHandle;
import android.util.Size;
import com.android.internal.app.IAppOpsCallback;
import com.android.internal.app.IAppOpsService;
import com.gesher.server.CameraService;
import com.gesher.server.LegacyCamera;
import java.lang.ref.WeakReference;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Exercises both production admission paths and live AppOps ownership.
 * Camera1 starts real preview/picture workers with a fixture-only frame source;
 * package, AppOps and device discovery boundaries are isolated from hardware.
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

    private static final class CameraOps extends IAppOpsService.Default {
        final Map<String, Integer> modes = new HashMap<>();
        final Map<IAppOpsCallback, String> watchers = new HashMap<>();
        final Map<IBinder, String> running = new HashMap<>();
        boolean failCheck;

        private int mode(int op, int uid, String name) {
            check(op == AppOpsManager.OP_CAMERA && uid == Process.myUid(), "camera AppOps operation owner");
            check(Binder.getCallingUid() == uid && Binder.getCallingPid() == Process.myPid(),
                    "camera AppOps query inherited remote identity");
            return modes.getOrDefault(name, AppOpsManager.MODE_ALLOWED);
        }

        @Override public int checkOperation(int op, int uid, String name) throws RemoteException {
            if (failCheck) throw new RemoteException("controlled AppOps check failure");
            return mode(op, uid, name);
        }

        @Override public void startWatchingModeWithFlags(int op, String name, int flags,
                IAppOpsCallback watcher) {
            check(op == AppOpsManager.OP_CAMERA && flags == AppOpsManager.WATCH_FOREGROUND_CHANGES,
                    "watch camera mode and effective foreground transitions");
            check(watchers.put(watcher, name) == null, "unique watcher per camera owner");
        }

        @Override public void stopWatchingMode(IAppOpsCallback watcher) {
            check(watchers.remove(watcher) != null, "remove the exact watcher once");
        }

        @Override public SyncNotedAppOp startOperation(IBinder token, int op, int uid, String name,
                String tag, boolean startDefault, boolean collect, String message,
                boolean collectMessage, int flags, int chain) {
            int mode = mode(op, uid, name);
            if (mode == AppOpsManager.MODE_ALLOWED) {
                check(running.put(token, name) == null, "start one token per open capture source");
            }
            return new SyncNotedAppOp(mode, op, tag, name);
        }

        @Override public void finishOperation(IBinder token, int op, int uid, String name, String tag) {
            mode(op, uid, name);
            check(name.equals(running.remove(token)), "finish the exact capture token once");
        }

        void notifyMode(String name, int mode) throws RemoteException {
            modes.put(name, mode);
            for (var watcher : new ArrayList<>(watchers.entrySet())) {
                if (name.equals(watcher.getValue())) {
                    watcher.getKey().opChanged(AppOpsManager.OP_CAMERA, Process.myUid(), name, "default:0");
                }
            }
        }
    }

    private static void activeCamera2(CameraService service, Packages packages, CameraOps ops)
            throws Exception {
        CountDownLatch disabled = new CountDownLatch(1);
        ICameraDeviceUser active = service.connectDevice(new ICameraDeviceCallbacks.Default() {
            @Override public void onDeviceError(int error, CaptureResultExtras extras) {
                if (error == ICameraDeviceCallbacks.ERROR_CAMERA_DISABLED) disabled.countDown();
            }
        }, "0", 0, 36, null, source(OWNER), 0, false);
        var owners = (Map<?, ?>) field(CameraService.class, "mOpen").get(service);
        Object owner = owners.get("0");
        try {
            ops.notifyMode(SIBLING, AppOpsManager.MODE_ERRORED);
            denied(service, source(SIBLING), "hard AppOps denial cannot evict an allowed owner");
            ops.notifyMode(SIBLING, AppOpsManager.MODE_IGNORED);
            denied(service, source(SIBLING), ICameraService.ERROR_DISABLED,
                    "soft AppOps denial cannot evict an allowed owner");
            check(ops.watchers.size() == 1 && owners.get("0") == owner && ops.running.isEmpty(),
                    "admission watches policy without starting capture use or changing owner");
            ops.failCheck = true;
            denied(service, source(SIBLING), ICameraService.ERROR_DISABLED, "failed AppOps authority");
            ops.failCheck = false;
            check(ops.watchers.size() == 1, "failed policy check removes its provisional watcher");

            // Invoke the service boundary used before Media Foundation opens the
            // source. The camera itself stays isolated from hardware on this host.
            Object user = field(owner.getClass(), "user").get(owner);
            Method start = CameraService.class.getDeclaredMethod("startCapture", String.class,
                    user.getClass());
            start.setAccessible(true);
            Method failed = CameraService.class.getDeclaredMethod("captureFailedToOpen", String.class,
                    user.getClass());
            failed.setAccessible(true);
            check((boolean) start.invoke(service, "0", user), "the allowed capture starts");
            check((boolean) start.invoke(service, "0", user) && ops.running.size() == 1,
                    "same camera source does not nest start accounting");
            failed.invoke(service, "0", user);
            check(ops.running.isEmpty(), "failed native open finishes source accounting");
            check((boolean) start.invoke(service, "0", user), "a failed native open can retry");

            IAppOpsCallback watcher = ops.watchers.keySet().iterator().next();
            watcher.opChanged(AppOpsManager.OP_CAMERA, Process.myUid() + 1, OWNER, "default:0");
            check(owners.get("0") == owner && ops.running.size() == 1, "ignore unrelated UID callback");
            ops.notifyMode(OWNER, AppOpsManager.MODE_IGNORED);
            check(disabled.await(5, TimeUnit.SECONDS), "active client receives CAMERA_DISABLED");
            check(owners.isEmpty() && ops.running.isEmpty() && ops.watchers.isEmpty(),
                    "revocation retires the owner, operation and watcher");
            try {
                active.beginConfigure();
                throw new AssertionError("disabled owner accepted a new camera configuration");
            } catch (ServiceSpecificException expected) {
                check(expected.errorCode == ICameraService.ERROR_DISCONNECTED, "disabled owner disconnected");
            }
            denied(service, source(OWNER), ICameraService.ERROR_DISABLED, "revoked owner cannot reopen");
            ops.notifyMode(OWNER, AppOpsManager.MODE_ALLOWED);
            ICameraDeviceUser superseded = camera2(service, source(OWNER));
            try {
                Object newOwner = owners.get("0");
                Object newUser = field(newOwner.getClass(), "user").get(newOwner);
                check((boolean) start.invoke(service, "0", newUser), "restored access starts on reopen");
                ICameraDeviceUser replacement = camera2(service, source(OWNER));
                try {
                    check(ops.watchers.size() == 1 && ops.running.isEmpty(),
                            "new owner finishes the old token and removes only the old watcher");
                    // Live permission loss is checked again when policy notifies us.
                    packages.granted = false;
                    ops.notifyMode(OWNER, AppOpsManager.MODE_ALLOWED);
                    check(owners.isEmpty() && ops.watchers.isEmpty(), "runtime permission loss disconnects");
                } finally {
                    replacement.disconnect();
                    packages.granted = true;
                }
            } finally {
                superseded.disconnect();
            }
        } finally {
            active.disconnect();
            ops.failCheck = false;
            ops.modes.clear();
        }
    }

    private static void installFrameFixture(Object session) throws Exception {
        Object choice = field(session.getClass(), "sourceChoice").get(session);
        Class<?> selection = Class.forName("com.gesher.server.CameraImageSource$Selection");
        Constructor<?> constructor = selection.getDeclaredConstructor(int.class, byte[].class);
        constructor.setAccessible(true);
        Object image = constructor.newInstance(field(selection, "IMAGE").getInt(null), new byte[] {1});
        Method set = choice.getClass().getDeclaredMethod("set", selection);
        Method keepFrame = choice.getClass().getDeclaredMethod("keepFrame", byte[].class, int.class, int.class);
        set.setAccessible(true);
        keepFrame.setAccessible(true);
        set.invoke(choice, image);
        keepFrame.invoke(choice, new byte[320 * 240 * 3 / 2], 320, 240);
    }

    private static void activeLegacy(Packages packages, CameraOps ops) throws Exception {
        var sessions = (Map<?, ?>) field(LegacyCamera.class, "sSessions").get(null);
        ops.notifyMode(OWNER, AppOpsManager.MODE_IGNORED);
        check(LegacyCamera.setup(null, new WeakReference<Camera>(null), 0) == -13,
                "legacy camera rejects a soft AppOps denial before publication");
        check(sessions.isEmpty() && ops.watchers.isEmpty(), "legacy denial retained a session or watcher");

        ops.notifyMode(OWNER, AppOpsManager.MODE_ALLOWED);
        check(LegacyCamera.setup(null, new WeakReference<Camera>(null), 0) == 0,
                "legacy camera opens after AppOps recovery");
        Object active = sessions.get(null);
        try {
            check(ops.watchers.size() == 1 && ops.running.isEmpty(), "legacy access watched before preview");
            installFrameFixture(active);
            LegacyCamera.startPreview(null);
            Field preview = field(active.getClass(), "capture");
            Object previewCapture = preview.get(active);
            check(previewCapture != null && LegacyCamera.previewEnabled(null) && ops.running.size() == 1,
                    "legacy preview owns one active camera operation");
            LegacyCamera.stopPreview(null);
            check(!LegacyCamera.previewEnabled(null) && ops.running.isEmpty()
                    && ops.watchers.size() == 1, "legacy stop finishes only the streaming operation");

            LegacyCamera.startPreview(null);
            IAppOpsCallback watcher = ops.watchers.keySet().iterator().next();
            watcher.opChanged(AppOpsManager.OP_CAMERA, Process.myUid() + 1, OWNER, "default:0");
            check(LegacyCamera.previewEnabled(null), "legacy preview ignores callbacks for other UIDs");
            ops.notifyMode(OWNER, AppOpsManager.MODE_IGNORED);
            check(ops.watchers.isEmpty() && ops.running.isEmpty() && preview.get(active) == null,
                    "revocation releases the legacy preview, operation and mode watcher");
            try {
                LegacyCamera.startPreview(null);
                throw new AssertionError("retired legacy client reopened the camera");
            } catch (RuntimeException expected) {
                check(true, "retired legacy client is disconnected");
            }
        } finally {
            LegacyCamera.release(null);
            ops.notifyMode(OWNER, AppOpsManager.MODE_ALLOWED);
            packages.granted = true;
        }

        Camera opened = (Camera) allocate(Camera.class);
        check(LegacyCamera.setup(opened, new WeakReference<>(opened), 0) == 0, "legacy release path opens");
        LegacyCamera.release(opened);
        check(sessions.isEmpty() && ops.watchers.isEmpty(), "explicit close runs the camera cleanup once");

        check(LegacyCamera.setup(null, new WeakReference<Camera>(null), 0) == 0, "new legacy connection");
        try {
            Object next = sessions.get(null);
            installFrameFixture(next);
            LegacyCamera.startPreview(null);
            LegacyCamera.takePicture(null, 0);
            Field picture = field(next.getClass(), "picture");
            Object pictureCapture = null;
            for (int i = 0; i < 500 && pictureCapture == null; ++i) {
                synchronized (next) { pictureCapture = picture.get(next); }
                if (pictureCapture == null) Thread.sleep(1);
            }
            check(pictureCapture != null && ops.running.size() == 1,
                    "the still owns the same operation after preview stops");
            packages.granted = false;
            ops.notifyMode(OWNER, AppOpsManager.MODE_ALLOWED);
            int result = (int) pictureCapture.getClass().getMethod("read", byte[].class)
                    .invoke(pictureCapture, new byte[320 * 240 * 3 / 2]);
            check(result != 0 && ops.watchers.isEmpty() && ops.running.isEmpty(),
                    "live runtime denial closes the still and revokes its operation");
        } finally {
            LegacyCamera.release(null);
            packages.granted = true;
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
        denied(service, source, ICameraService.ERROR_PERMISSION_DENIED, reason);
    }

    private static void denied(CameraService service, AttributionSourceState source, int error,
            String reason) throws Exception {
        Object previous = ((Map<?, ?>) field(CameraService.class, "mOpen").get(service)).get("0");
        try {
            ICameraDeviceUser unexpected = camera2(service, source);
            if (unexpected != null) unexpected.disconnect();
            throw new AssertionError(reason + ": Camera2 admitted the caller");
        } catch (ServiceSpecificException expected) {
            check(expected.errorCode == error,
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
        CameraOps ops = new CameraOps();
        @SuppressWarnings("unchecked")
        Map<String, IBinder> services = (Map<String, IBinder>) field(ServiceManager.class, "sCache").get(null);
        Binder opsBinder = new Binder();
        opsBinder.attachInterface(ops, IAppOpsService.class.getName());
        IBinder oldOps = services.put(Context.APP_OPS_SERVICE, opsBinder);
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
            activeCamera2(service, packages, ops);
            activeLegacy(packages, ops);
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
            if (oldOps == null) services.remove(Context.APP_OPS_SERVICE);
            else services.put(Context.APP_OPS_SERVICE, oldOps);
            manager.set(null, oldManager);
            current.set(null, oldThread);
            devices.set(service, oldDevices);
        }
        System.out.println("CameraPermissionContractTest: both admission paths, live grants and UID attribution passed ("
                + checks + " checks; includes Camera2 and legacy AppOps lifetime and revoke)");
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        contracts();
    }
}
