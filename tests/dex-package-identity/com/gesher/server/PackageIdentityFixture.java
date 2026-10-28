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
 * Starts the real private native bootstrap, then runs the install/child probes in
 * its dedicated owned writer VM. Requires exact rebuilt binderd/servicemanager.
 */
public final class PackageIdentityFixture {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    public static void main(String[] args) throws Exception {
        String root = System.getenv("ANDROID_ROOT");
        if (root == null) root = "/system";
        com.gesher.app.Main.bootstrap("uid-fixture", root + "/build.prop",
                root + "/framework/framework-res.apk");
        check(args.length == 3 || args.length == 5, "requires two APKs and an update of A");
        if (!Zygote.nativeIsAuthority()) {
            check(args.length == 3, "private writer bootstrap required for inherited arguments");
            check(Zygote.joinJob(0), "fixture root kill-on-close job");
            String exe = Zygote.exePath();
            String dir = exe.substring(0, exe.lastIndexOf('\\') + 1);
            String device = "uid-fixture-" + java.util.UUID.randomUUID();
            android.system.Os.setenv("GESHER_BINDER", device, true);
            String vm = Zygote.vmCommandLine(PackageIdentityFixture.class.getName());
            File logs = new File(Device.dataDir(), "bootstrap");
            check(logs.mkdirs() || logs.isDirectory(), "bootstrap log directory");
            int[] owned = Zygote.startDevice(List.of(
                    Zygote.quote(dir + "binderd.exe") + " " + device,
                    Zygote.quote(dir + "servicemanager.exe"),
                    Zygote.commandLine(vm, PackageIdentityFixture.class.getName(),
                            List.of(args[0], args[1], args[2]))),
                    List.of(new File(logs, "broker.log").getPath(),
                            new File(logs, "manager.log").getPath(),
                            new File(logs, "writer.log").getPath()),
                    List.of("GESHER_BINDER=" + device));
            long deadline = android.os.SystemClock.uptimeMillis() + 180_000;
            long code;
            while ((code = Zygote.exitStatus(owned[2])) == Zygote.EXIT_RUNNING) {
                check(android.os.SystemClock.uptimeMillis() < deadline, "writer timeout");
                Thread.sleep(20);
            }
            Zygote.kill(owned[0]); Zygote.kill(owned[1]);
            check(code == 0, "dedicated writer fixture failed: " + code);
            check(java.nio.file.Files.readString(new File(logs, "writer.log").toPath())
                    .contains("PACKAGE_INSTALL_UID_IDENTITY_PASS"), "writer result missing");
            System.out.println("PRIVATE_BOOTSTRAP_PACKAGE_UID_PASS");
            System.exit(0);
        }
        check(args.length == 5 && "--adopt".equals(args[3]), "owned bootstrap adoption");
        check(Zygote.joinJob(0), "writer lifetime job");
        for (String pid : args[4].split(",")) check(Zygote.joinJob(Integer.parseInt(pid)), "owned child job");
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
        android.os.ServiceManager.addService("gesher.uid.fixture", new android.os.Binder() {
            @Override protected boolean onTransact(int code, android.os.Parcel data,
                    android.os.Parcel reply, int flags) {
                int expected = data.readInt();
                int actual = android.os.Binder.getCallingUid();
                check((expected == first.uid || expected == other.uid) && actual == expected,
                        "remote Binder caller UID must equal PM authority");
                reply.writeInt(actual);
                return true;
            }
        });
        SystemServer.startBinderThreads("uid-fixture", 3);
        check(first.uid != other.uid, "distinct installed packages must have distinct uids");
        // Real VMs for both identities, with deliberately wrong inherited/replaced
        // UID environment. Includes B, whose nondefault UID caught the peer defect.
        childUid(pm, first);
        childUid(pm, other);
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

    private static void childUid(PackageManagerService pm, PackageIdentity identity)
            throws Exception {
        check(pm.getApplicationInfo(identity.packageName, 0, 0).uid == identity.uid
                && pm.getPackageUid(identity.packageName, 0, 0) == identity.uid,
                "PM UID agrees before real child launch");
        String vm = Zygote.vmCommandLine(PackageIdentityFixture.class.getName());
        File output = new File(Device.dataDir(), "child-" + identity.uid + ".log");
        int pid = Zygote.spawnPackage(Zygote.commandLine(vm, PackageUidChild.class.getName(),
                List.of(String.valueOf(identity.uid), "gesher.uid.fixture")),
                List.of("GESHER_UID=19999", "GESHER_BINDER_UID=1000"),
                output.getPath(), identity);
        long deadline = android.os.SystemClock.uptimeMillis() + 30_000;
        long code;
        while ((code = Zygote.exitStatus(pid)) == Zygote.EXIT_RUNNING) {
            if (android.os.SystemClock.uptimeMillis() > deadline) {
                Zygote.kill(pid);
                throw new AssertionError("real child UID probe timed out");
            }
            Thread.sleep(20);
        }
        check(code == 0, "real child UID probe failed: " + code);
        check(java.nio.file.Files.readString(output.toPath()).contains(
                "CHILD_UID_PASS " + identity.uid), "child UID result missing");
    }
}
