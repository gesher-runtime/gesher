/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.app.AppOpsManager;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageManager;
import android.os.IBinder;
import android.os.Process;
import android.os.ServiceManager;
import android.os.UserHandle;
import com.android.internal.app.IAppOpsService;
import com.android.internal.os.ApplicationSharedMemory;
import com.gesher.app.Main;
import com.gesher.server.ApkPackage;
import com.gesher.server.AppOpsService;
import com.gesher.server.CallerIdentity;
import com.gesher.server.Device;
import com.gesher.server.PackageManagerService;
import java.lang.reflect.Method;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.function.Supplier;

/** Explicit test-operator special access, through the real persistent AppOps service. */
public final class FixtureSettingsSetup {
    private static void require(boolean value, String why) { if (!value) throw new AssertionError(why); }
    private static AppOpsService attach(PackageManagerService packages) throws Exception {
        AppOpsService ops = packages.appOps();
        Method method = AppOpsService.class.getDeclaredMethod("attach", CallerIdentity.class, Supplier.class);
        method.setAccessible(true);
        // Special access does not depend on foreground state. This server-local
        // operator has no application processes; no UID/focus is manufactured.
        method.invoke(ops, new CallerIdentity(packages, null), (Supplier<Map<Integer, ?>>)Collections::emptyMap);
        return ops;
    }
    public static void main(String[] args) throws Exception {
        boolean remote = args.length > 0 && args[0].equals("--remote");
        if (remote ? args.length < 3 || args.length > 4 : args.length != 1) throw new IllegalArgumentException("APK or --remote UID PACKAGE [MODE] required");
        Properties state = null;
        if (remote) {
            state = Device.read(Device.stateFile());
            String device = state == null ? null : state.getProperty(Device.STATE_DEVICE);
            require(device != null && device.matches("gesher-[A-Za-z0-9-]+"), "Real fixture device is unavailable");
            android.system.Os.setenv("GESHER_BINDER", device, true);
        }
        String root = System.getenv("ANDROID_ROOT");
        Main.bootstrap("fixture-write-settings", root + "/build.prop", root + "/framework/framework-res.apk");
        if (Process.myUid() != Process.SYSTEM_UID) throw new SecurityException("Special-access operator requires system UID");
        if (remote) {
            require(Device.alive(state, Device.SYSTEM_SERVER) && Device.alive(state, Device.BINDERD), "Real fixture device generation is stale");
            int uid = Integer.parseInt(args[1]); String name = args[2];
            int mode = args.length == 3 ? AppOpsManager.MODE_ALLOWED : Integer.parseInt(args[3]);
            require(mode == AppOpsManager.MODE_ALLOWED || mode == AppOpsManager.MODE_IGNORED
                    || mode == AppOpsManager.MODE_ERRORED || mode == AppOpsManager.MODE_DEFAULT, "Bounded special-access fixture mode required");
            require(UserHandle.isApp(uid), "Ordinary application UID required");
            IBinder packageBinder = ServiceManager.getService("package"), opsBinder = ServiceManager.getService("appops");
            require(packageBinder != null && opsBinder != null
                    && packageBinder.getClass().getName().equals("android.os.BinderProxy")
                    && opsBinder.getClass().getName().equals("android.os.BinderProxy"), "Actual remote policy services required");
            IPackageManager packages = IPackageManager.Stub.asInterface(packageBinder);
            IAppOpsService ops = IAppOpsService.Stub.asInterface(opsBinder);
            ApplicationInfo installed = packages.getApplicationInfo(name, 0, 0);
            require(installed != null && installed.uid == uid && name.equals(installed.packageName)
                    && packages.getPackageUid(name, 0, 0) == uid, "Installed special-access subject changed");
            android.content.pm.PackageInfo info = packages.getPackageInfo(name, PackageManager.GET_PERMISSIONS, 0);
            require(info != null && info.requestedPermissions != null
                    && java.util.Arrays.asList(info.requestedPermissions).contains(Manifest.permission.WRITE_SETTINGS), "Fixture must declare special access");
            require(packages.checkPermission(Manifest.permission.WRITE_SETTINGS, name, 0) == PackageManager.PERMISSION_DENIED,
                    "Special access fixture must not hold a signature/role grant");
            ops.setMode(AppOpsManager.OP_WRITE_SETTINGS, uid, name, mode);
            require(ops.checkOperation(AppOpsManager.OP_WRITE_SETTINGS, uid, name) == mode, "Live special-access readback failed");
            require(packages.checkPermission(Manifest.permission.WRITE_SETTINGS, name, 0) == PackageManager.PERMISSION_DENIED,
                    "AppOps must not fabricate a signature grant");
            System.out.println("FixtureSettingsSetup live special access UID " + uid + " package " + name + " source " + installed.sourceDir + " mode " + mode);
        } else {
            ApplicationSharedMemory.setInstance(ApplicationSharedMemory.create());
            ApkPackage apk = ApkPackage.parse(args[0]);
            require(apk.hasVerifiedSigningIdentity(), "Special access requires the fixture's real verified APK signer");
            PackageManagerService packages = new PackageManagerService(List.of(apk));
            require(UserHandle.isApp(apk.uid()), "Ordinary admitted application required");
            require(apk.parsed.getRequestedPermissions().contains(Manifest.permission.WRITE_SETTINGS), "Fixture must declare special access");
            require(packages.checkPermission(Manifest.permission.WRITE_SETTINGS, apk.packageName, 0) == PackageManager.PERMISSION_DENIED,
                    "Special access fixture must not hold a signature/role grant");
            AppOpsService ops = attach(packages);
            ops.setMode(AppOpsManager.OP_WRITE_SETTINGS, apk.uid(), apk.packageName, AppOpsManager.MODE_ALLOWED);
            require(ops.checkOperation(AppOpsManager.OP_WRITE_SETTINGS, apk.uid(), apk.packageName) == AppOpsManager.MODE_ALLOWED, "Special-access readback failed");
            // setMode publishes an asynchronous policy snapshot. Cross the real
            // operator durability boundary before opening a second service/store.
            ops.reloadNonHistoricalState();
            ApkPackage reopened = ApkPackage.parse(args[0]);
            require(reopened.hasVerifiedSigningIdentity()
                    && java.util.Objects.equals(apk.verifiedSignerSha256(), reopened.verifiedSignerSha256()),
                    "Special-access signer changed on reopen");
            PackageManagerService readback = new PackageManagerService(List.of(reopened));
            require(reopened.uid() == apk.uid(), "Special-access subject changed on reopen");
            require(attach(readback).checkOperation(AppOpsManager.OP_WRITE_SETTINGS, reopened.uid(), reopened.packageName) == AppOpsManager.MODE_ALLOWED,
                    "Special access did not survive the real store reload");
            require(readback.checkPermission(Manifest.permission.WRITE_SETTINGS, reopened.packageName, 0) == PackageManager.PERMISSION_DENIED,
                    "Special access must remain distinct from the signature permission");
            System.out.println("FixtureSettingsSetup persisted special access UID " + apk.uid() + " package " + apk.packageName);
        }
        System.out.flush(); System.exit(0);
    }
}
