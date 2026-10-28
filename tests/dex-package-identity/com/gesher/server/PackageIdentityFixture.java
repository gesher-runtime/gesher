/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.io.File;
import java.util.List;

/**
 * Controlled real install fixture, to compile against the exact rebuilt gesher-app and
 * framework classes. Run only in an owned, empty ANDROID_DATA whose final directory name
 * starts with uid-identity-fixture-. Arguments: package-A.apk package-B.apk update-A.apk.
 * This exercises installer records and PM, NOT Binder transport authentication.
 */
public final class PackageIdentityFixture {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    public static void main(String[] args) throws Exception {
        check(args.length == 3, "requires two distinct real APKs and a real update of A");
        File data = new File(Device.dataDir());
        check(data.getName().startsWith("uid-identity-fixture-"), "owned fixture directory required");
        check(!new File(data, "system/packages.list").exists(), "requires empty install registry");
        String a = ApkPackage.parse(args[0]).packageName;
        String b = ApkPackage.parse(args[1]).packageName;
        check(!a.equals(b), "A and B must be different packages");
        check(a.equals(ApkPackage.parse(args[2]).packageName), "update must belong to A");
        PackageManagerService pm = new PackageManagerService(List.of());
        Installer installer = Installer.create(pm, new ActivityTaskManagerService(pm));
        check("installed".equals(installer.install(args[0])[1]), "initial A install");
        check("installed".equals(installer.install(args[1])[1]), "initial B install");
        PackageIdentity first = pm.launchIdentity(a);
        PackageIdentity other = pm.launchIdentity(b);
        check(first.uid != other.uid, "distinct installed packages must have distinct uids");
        check(pm.getPackageUid(a, 0, 0) == first.uid, "getPackageUid");
        check(pm.getApplicationInfo(a, 0, 0).uid == first.uid, "ApplicationInfo uid");
        check(a.equals(pm.getNameForUid(first.uid)), "exact UID name");
        check(java.util.Arrays.equals(pm.getPackagesForUid(first.uid), new String[] {a}),
                "UID lookup must not expose B");
        check(pm.getNameForUid(1000) == null && pm.getPackagesForUid(1000) == null,
                "system UID is not an installed app");
        try {
            pm.getPackageUid(a, 0, 1);
            throw new AssertionError("unsupported user admitted");
        } catch (IllegalArgumentException expected) {
            // Explicitly unsupported; no user-0 fallback.
        }
        check("updated".equals(installer.install(args[2])[1]), "requires changed update APK");
        PackageIdentity update = pm.launchIdentity(a);
        check(update.uid == first.uid && update.installInstance.equals(first.installInstance),
                "update preserves identity");
        PackageManagerService restart = new PackageManagerService(Installer.loadInstalled());
        PackageIdentity afterRestart = restart.launchIdentity(a);
        check(afterRestart.uid == first.uid
                && afterRestart.installInstance.equals(first.installInstance), "restart identity");
        check(installer.uninstall(a), "uninstall A");
        check(pm.getNameForUid(first.uid) == null, "removed UID mapping");
        check("installed".equals(installer.install(args[0])[1]), "reinstall A");
        check(!pm.launchIdentity(a).installInstance.equals(first.installInstance),
                "reinstall must not revive old install instance even if uid is reused");
        check(pm.launchIdentity(b).uid == other.uid, "B identity remains stable");
        check(installer.uninstall(a) && installer.uninstall(b), "owned installs removed");
        System.out.println("PACKAGE_INSTALL_UID_IDENTITY_PASS (transport authority remains unqualified)");
        System.exit(0);
    }
}
