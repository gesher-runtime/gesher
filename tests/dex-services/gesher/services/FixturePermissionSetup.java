/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.pm.PackageManager;
import android.content.pm.IPackageManager;
import android.content.pm.ApplicationInfo;
import android.os.IBinder;
import android.os.Process;
import android.os.ServiceManager;
import android.os.UserHandle;
import android.permission.IPermissionManager;
import com.android.internal.os.ApplicationSharedMemory;
import com.gesher.app.Main;
import com.gesher.server.ApkPackage;
import com.gesher.server.CallerIdentity;
import com.gesher.server.Device;
import com.gesher.server.PackageManagerService;
import com.gesher.server.PermissionPolicy;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.List;
import java.util.Properties;

/** Explicit test-operator grants for a real APK in the isolated Windows test profile. */
public final class FixturePermissionSetup {
    public static void main(String[] args) throws Exception {
        if (args.length < 2) throw new IllegalArgumentException("APK and requested permissions required");
        boolean remote = args[0].equals("--remote");
        Properties state = null;
        if (remote) {
            if (args.length < 4) throw new IllegalArgumentException("--remote UID PACKAGE PERMISSION... required");
            // Select this profile's actual running device before bootstrap can open Binder.
            state = Device.read(Device.stateFile());
            String device = state == null ? null : state.getProperty(Device.STATE_DEVICE);
            if (device == null || !device.matches("gesher-[A-Za-z0-9-]+")) {
                throw new IllegalStateException("Installed fixture device is unavailable");
            }
            android.system.Os.setenv("GESHER_BINDER", device, true);
        }
        String root = System.getenv("ANDROID_ROOT");
        Main.bootstrap("fixture-permissions", root + "/build.prop", root + "/framework/framework-res.apk");
        if (Process.myUid() != Process.SYSTEM_UID) throw new SecurityException("Test operator requires system UID");
        if (remote) {
            if (!Device.alive(state, Device.SYSTEM_SERVER) || !Device.alive(state, Device.BINDERD)) {
                throw new IllegalStateException("Installed fixture device identity is stale");
            }
            grantInstalled(args);
            System.out.flush();
            System.exit(0);
        }
        ApplicationSharedMemory.setInstance(ApplicationSharedMemory.create());
        ApkPackage apk = ApkPackage.parse(args[0]);
        PackageManagerService packages = new PackageManagerService(List.of(apk));
        if (!UserHandle.isApp(apk.uid())) throw new AssertionError("APK was not admitted with an application UID");
        PermissionPolicy policy = packages.permissions();
        // Same server-local policy wiring as production; nothing is exposed to the APK.
        Method attach = PermissionPolicy.class.getDeclaredMethod("attach", CallerIdentity.class);
        attach.setAccessible(true);
        attach.invoke(policy, new CallerIdentity(packages, null));
        for (int i = 1; i < args.length; ++i) {
            policy.grantRuntimePermission(apk.packageName, args[i], "default:0", 0);
            if (packages.checkPermission(args[i], apk.packageName, 0) != PackageManager.PERMISSION_GRANTED) {
                throw new AssertionError("Requested fixture permission was not granted: " + args[i]);
            }
        }
        // Await the real durable write, then prove a fresh policy instance reads it.
        Field writerField = PermissionPolicy.class.getDeclaredField("mWriter");
        writerField.setAccessible(true);
        Object writer = writerField.get(policy);
        Method flush = writer.getClass().getDeclaredMethod("flush");
        flush.setAccessible(true);
        if (!Boolean.TRUE.equals(flush.invoke(writer))) throw new AssertionError("Permission persistence failed");
        ApkPackage reopened = ApkPackage.parse(args[0]);
        PackageManagerService readback = new PackageManagerService(List.of(reopened));
        if (reopened.uid() != apk.uid()) throw new AssertionError("Canonical APK identity changed on reopen");
        for (int i = 1; i < args.length; ++i) {
            if (readback.checkPermission(args[i], reopened.packageName, 0) != PackageManager.PERMISSION_GRANTED) {
                throw new AssertionError("Permission did not survive policy reload: " + args[i]);
            }
        }
        System.out.println("FixturePermissionSetup UID " + apk.uid() + " package " + apk.packageName);
        System.out.println("FixturePermissionSetup persisted grants verified");
        System.out.flush();
        System.exit(0);
    }

    private static void grantInstalled(String[] args) throws Exception {
        int expectedUid = Integer.parseInt(args[1]);
        String name = args[2];
        if (!UserHandle.isApp(expectedUid)) throw new IllegalArgumentException("Expected an application UID");
        IBinder packageBinder = ServiceManager.getService("package");
        IBinder permissionBinder = ServiceManager.getService("permissionmgr");
        if (packageBinder == null || permissionBinder == null
                || !packageBinder.getClass().getName().equals("android.os.BinderProxy")
                || !permissionBinder.getClass().getName().equals("android.os.BinderProxy")) {
            throw new AssertionError("Installed grants require actual remote package and permission services");
        }
        IPackageManager packages = IPackageManager.Stub.asInterface(packageBinder);
        IPermissionManager policy = IPermissionManager.Stub.asInterface(permissionBinder);
        ApplicationInfo installed = packages.getApplicationInfo(name, 0, 0);
        if (installed == null || installed.uid != expectedUid || !name.equals(installed.packageName)
                || packages.getPackageUid(name, 0, 0) != expectedUid) {
            throw new AssertionError("Installed fixture identity differs from its admitted APK");
        }
        for (int i = 3; i < args.length; ++i) {
            // An explicit test-operator decision after real Installer publication.
            // Fresh installation correctly does not inherit a standalone PM's grants.
            policy.grantRuntimePermission(name, args[i], "default:0", 0);
            if (packages.checkPermission(args[i], name, 0) != PackageManager.PERMISSION_GRANTED) {
                throw new AssertionError("Remote installed permission readback failed: " + args[i]);
            }
        }
        System.out.println("FixturePermissionSetup remote UID " + installed.uid + " package " + name
                + " source " + installed.sourceDir);
        System.out.println("FixturePermissionSetup live installed grants verified");
    }
}
