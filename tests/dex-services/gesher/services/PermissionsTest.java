/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.content.Context;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.content.res.AssetManager;
import java.lang.reflect.Method;
import java.util.Collections;
import java.util.List;

/** Offline checks against actual framework metadata and persisted package decisions. */
public final class PermissionsTest {
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        Class<?> pmClass = Class.forName("com.gesher.server.PackageManagerService");
        Class<?> policyClass = Class.forName("com.gesher.server.PermissionPolicy");
        Object emptyPm = pmClass.getConstructor(java.util.Collection.class).newInstance(Collections.emptyList());
        Object policy = pmClass.getMethod("permissions").invoke(emptyPm);
        Method metadata = policyClass.getMethod("getPermissionInfo", String.class, String.class, int.class);
        PermissionInfo location = (PermissionInfo) metadata.invoke(policy, "android.permission.ACCESS_FINE_LOCATION", "android", 0);
        require(location != null && "android".equals(location.packageName), "framework permission definition missing");
        require((location.protectionLevel & PermissionInfo.PROTECTION_MASK_BASE) == PermissionInfo.PROTECTION_DANGEROUS,
                "dangerous protection level lost while reading compiled framework manifest");
        require(metadata.invoke(policy, "android.permission.NOT_A_REAL_PERMISSION", "android", 0) == null,
                "invented metadata for an unknown permission");
        Method check = policyClass.getMethod("checkPermission", String.class, String.class, String.class, int.class);
        require((int)check.invoke(policy, "com.example.untrusted", "android.permission.FAKE_PACKAGE_SIGNATURE", "default:0", 0)
                == PackageManager.PERMISSION_DENIED, "unknown caller received signature spoofing permission");
        require((int)check.invoke(policy, "com.example.untrusted", "android.permission.INTERNET", "default:0", 0)
                == PackageManager.PERMISSION_DENIED, "unknown package received a normal permission");
        require((int)pmClass.getMethod("checkSignatures",String.class,String.class,int.class)
                .invoke(emptyPm,"missing.one","missing.two",0) == PackageManager.SIGNATURE_UNKNOWN_PACKAGE,
                "unknown packages matched signatures");
        System.out.println("PermissionsTest: real framework metadata and unknown package denials passed");

        Class<?> profiles = Class.forName("com.gesher.server.Services$CrossProfileApps");
        Object cross = profiles.getConstructor().newInstance();
        require(!(boolean) profiles.getMethod("canRequestInteractAcrossProfiles",String.class)
                .invoke(cross,"com.example.untrusted"),"single-user device exposed a work profile request");
        require(((List<?>)profiles.getMethod("getTargetUserProfiles",String.class)
                .invoke(cross,"com.example.untrusted")).isEmpty(),"invented cross-profile target");
        System.out.println("PermissionsTest: single-user CrossProfileApps reports no target profiles");

        if (args.length > 0) {
            Class<?> apk = Class.forName("com.gesher.server.ApkPackage");
            Object gms = apk.getMethod("parse",String.class).invoke(null,args[0]);
            Object pm = pmClass.getConstructor(java.util.Collection.class).newInstance(Collections.singletonList(gms));
            policy = pmClass.getMethod("permissions").invoke(pm);
            Class<?> callersClass = Class.forName("com.gesher.server.CallerIdentity");
            Object callers = callersClass.getConstructor(pmClass,Class.forName("com.gesher.server.ActivityTaskManagerService"))
                    .newInstance(pm,null);
            Method attach = policyClass.getDeclaredMethod("attach",callersClass);attach.setAccessible(true);attach.invoke(policy,callers);
            String pkg="com.google.android.gms", permission="android.permission.ACCESS_FINE_LOCATION";
            require((int)check.invoke(policy,pkg,permission,"default:0",0)==PackageManager.PERMISSION_DENIED,
                    "real microG mode silently granted a dangerous permission");
            require((int)check.invoke(policy,pkg,"android.permission.INTERNET","default:0",0)==PackageManager.PERMISSION_GRANTED,
                    "requested normal permission missing");
            Method grant=policyClass.getMethod("grantRuntimePermission",String.class,String.class,String.class,int.class);
            Method revoke=policyClass.getMethod("revokeRuntimePermission",String.class,String.class,String.class,int.class,String.class);
            try {
                grant.invoke(policy,pkg,permission,"default:0",0);
                require((int)check.invoke(policy,pkg,permission,"default:0",0)==PackageManager.PERMISSION_GRANTED,"explicit host grant not applied");
                Object reopenedPm=pmClass.getConstructor(java.util.Collection.class).newInstance(Collections.singletonList(gms));
                Object reopened=pmClass.getMethod("permissions").invoke(reopenedPm);
                require((int)check.invoke(reopened,pkg,permission,"default:0",0)==PackageManager.PERMISSION_GRANTED,"runtime grant did not persist");
                require((int)check.invoke(reopened,pkg,permission,"default:0",10)==PackageManager.PERMISSION_DENIED,"grant leaked to another user ID");
            } finally { revoke.invoke(policy,pkg,permission,"default:0",0,"regression test cleanup"); }
            require((int)check.invoke(policy,pkg,permission,"default:0",0)==PackageManager.PERMISSION_DENIED,"explicit revoke not applied");
            int state=(int)policyClass.getMethod("getPermissionRequestState",String.class,String.class,int.class)
                    .invoke(policy,pkg,permission,0);
            require(state==Context.PERMISSION_REQUEST_STATE_REQUESTABLE,"dangerous permission unavailable to host consent flow");
            require(!(boolean)policyClass.getMethod("canRequest",String.class,String.class)
                    .invoke(policy,pkg,"android.permission.FAKE_PACKAGE_SIGNATURE"),"host prompt can grant signature spoofing");
            require(pmClass.getMethod("getPackageInfo",String.class,long.class,int.class).invoke(pm,"com.android.vending",0L,0)==null,
                    "synthetic Companion masqueraded as installed beside real GmsCore");
            System.out.println("PermissionsTest: real microG grants, persistence, revoke and Companion absence passed");
        }
        System.out.println("PermissionsTest all passed");
    }
}
