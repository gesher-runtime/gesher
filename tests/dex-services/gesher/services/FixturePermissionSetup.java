/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.pm.PackageManager;
import android.os.Process;
import android.os.UserHandle;
import com.android.internal.os.ApplicationSharedMemory;
import com.gesher.app.Main;
import com.gesher.server.ApkPackage;
import com.gesher.server.CallerIdentity;
import com.gesher.server.PackageManagerService;
import com.gesher.server.PermissionPolicy;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.List;

/** Explicit test-operator grants for a real APK in the isolated Windows test profile. */
public final class FixturePermissionSetup {
    public static void main(String[] args) throws Exception {
        if (args.length < 2) throw new IllegalArgumentException("APK and requested permissions required");
        String root = System.getenv("ANDROID_ROOT");
        Main.bootstrap("fixture-permissions", root + "/build.prop", root + "/framework/framework-res.apk");
        if (Process.myUid() != Process.SYSTEM_UID) throw new SecurityException("Test operator requires system UID");
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
}
