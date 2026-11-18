/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.content.pm.Signature;
import android.content.pm.SigningDetails;
import android.os.Process;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedPermissionImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.CallerIdentity;
import com.gesher.server.PackageManagerService;
import com.gesher.server.PackageUids;
import com.gesher.server.PermissionPolicy;
import com.gesher.server.SharedLibraryGraph;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.lang.reflect.Field;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Properties;
import java.util.Set;
import sun.misc.Unsafe;

/** Contract tests for Roadmap Item F07: Per-Package Identity & UID Isolation. */
public final class PackageUidIsolationContractTest {
    private static Unsafe unsafe;
    private static int checks = 0;
    private static Signature[] verifiedFixture;
    private static boolean art;

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }

    private static void set(Object target, String name, Object value) throws Exception {
        Class<?> current = target.getClass();
        while (current != null) {
            try {
                Field f = current.getDeclaredField(name);
                f.setAccessible(true);
                f.set(target, value);
                return;
            } catch (NoSuchFieldException ignored) {
                current = current.getSuperclass();
            }
        }
        throw new NoSuchFieldException(name);
    }

    private static ApkPackage createPackage(String name, int uid, String sharedUserId,
            String... permissions) throws Exception {
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        parsed.setTargetSdkVersion(36);
        if (sharedUserId != null && !sharedUserId.isEmpty()) {
            parsed.setSharedUserId(sharedUserId);
        }
        if (permissions != null) {
            for (String p : permissions) {
                ParsedPermissionImpl perm = new ParsedPermissionImpl();
                perm.setName(p);
                perm.setPackageName(name);
                perm.setProtectionLevel(PermissionInfo.PROTECTION_DANGEROUS);
                parsed.addPermission(perm);
                parsed.addUsesPermission(new ParsedUsesPermissionImpl(p, 0, Set.of()));
            }
        }
        parsed.hideAsFinal();
        ApkPackage code = (ApkPackage) unsafe.allocateInstance(ApkPackage.class);
        set(code, "packageName", name);
        set(code, "parsed", parsed);
        set(code, "mUid", uid);
        set(code, "apkPath", "/fixture/" + name + "/base.apk");
        set(code, "mDataDir", "/fixture/" + name + "/data");
        set(code, "sharedLibraries", SharedLibraryGraph.EMPTY);
        return code;
    }

    private static ApkPackage pending(String name, String group, Signature[] signatures) throws Exception {
        ApkPackage pkg = createPackage(name, -1, group);
        set(pkg, "mUidPending", true);
        set(pkg, "mSigningIdentityChecked", true);
        set(pkg, "mVerifiedSignatures", signatures);
        set(pkg, "mVerifiedSignatureSchemeVersion", SigningDetails.SignatureSchemeVersion.SIGNING_BLOCK_V2);
        return pkg;
    }

    private static ApkPackage signedPending(String name, String group, File fixture) throws Exception {
        ApkPackage pkg = pending(name, group, null);
        set(pkg, "apkPath", fixture.getCanonicalPath());
        set(pkg, "mSigningIdentityChecked", false);
        return pkg;
    }

    private static int admitShared(String name, String group) throws Exception {
        ApkPackage pkg = pending(name, group, verifiedFixture);
        new PackageManagerService(List.of(pkg));
        check(pkg.applicationInfo(0).uid == pkg.uid() && pkg.uid() >= 10000,
                "Application metadata must expose the admitted UID, not the pending parser identity");
        return pkg.uid();
    }

    private interface Call { void run() throws Exception; }
    private static void denied(Call call, String message) throws Exception {
        try { call.run(); }
        catch (java.lang.reflect.InvocationTargetException wrapped) {
            if (wrapped.getCause() instanceof SecurityException) { checks++; return; }
            throw wrapped;
        }
        catch (SecurityException expected) { checks++; return; }
        throw new AssertionError(message);
    }
    private static Properties registry(File data) throws Exception {
        Properties result = new Properties();
        File path = new File(data, "system/package-uids");
        if (path.isFile()) try (FileInputStream input = new FileInputStream(path)) { result.load(input); }
        return result;
    }
    private static Object prepare(ApkPackage pkg) throws Exception {
        var method = PackageUids.class.getDeclaredMethod("prepare", java.util.Collection.class, java.util.Collection.class);
        method.setAccessible(true); return method.invoke(null, List.of(pkg), List.of());
    }
    private static void transaction(Object value, String method) throws Exception {
        var call = value.getClass().getDeclaredMethod(method); call.setAccessible(true); call.invoke(value);
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 1) throw new IllegalArgumentException("Pass a valid signed APK fixture");
        run(true, args[0]);
    }

    public static void run(boolean onArt, String signedFixture) throws Exception {
        art = onArt;
        if (art) {
            System.loadLibrary("android_runtime");
            android.content.res.AssetManager.createSystemAssetsInZygoteLocked(false,
                    System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        }
        Field f = Unsafe.class.getDeclaredField("theUnsafe");
        f.setAccessible(true);
        unsafe = (Unsafe) f.get(null);

        File tempDir = art ? Files.createTempDirectory("gesher-uid-test-").toFile()
                : new File(System.getenv("ANDROID_DATA"));
        Files.createDirectories(tempDir.toPath());
        try {
            if (art) android.system.Os.setenv("ANDROID_DATA", tempDir.getAbsolutePath(), true);
            // Verify default non-GMS mode still performs distinct UID allocation
            verifiedFixture = ApkPackage.verifySignatures(signedFixture);
            check(verifiedFixture != null && verifiedFixture.length != 0, "Fixture signing identity must verify");
            if (art) {
                ApkPackage archive = ApkPackage.parse(signedFixture);
                check(archive.uid() == -1, "Parsing an archive must not allocate an installed UID");
                check(!new File(tempDir, "system/package-uids").exists(), "Archive parsing must not persist identity membership");
            }

            // Test 1: Distinct UID allocation for standard packages
            int uid1 = PackageUids.uidFor("com.example.app1");
            int uid2 = PackageUids.uidFor("com.example.app2");
            int uid3 = PackageUids.uidFor("com.example.app3");

            check(uid1 >= 10000 && uid1 <= 19999, "uid1 must be in application UID range [10000, 19999]");
            check(uid2 >= 10000 && uid2 <= 19999, "uid2 must be in application UID range [10000, 19999]");
            check(uid3 >= 10000 && uid3 <= 19999, "uid3 must be in application UID range [10000, 19999]");
            check(uid1 != uid2, "Two different standard packages must get distinct UIDs");
            check(uid1 != uid3 && uid2 != uid3, "All three standard packages must get distinct UIDs");

            // Test 2: Stability of allocated UIDs across subsequent calls
            check(PackageUids.uidFor("com.example.app1") == uid1, "UID allocation for app1 must be stable");
            check(PackageUids.uidFor("com.example.app2") == uid2, "UID allocation for app2 must be stable");
            check(PackageUids.uidFor("com.example.app3") == uid3, "UID allocation for app3 must be stable");

            // Test 3: Shared UID allocation with android:sharedUserId
            String sharedGroup = "com.example.shared.group";
            int sharedUid1 = admitShared("com.example.shared.a", sharedGroup);
            int sharedUid2 = admitShared("com.example.shared.b", sharedGroup);

            check(sharedUid1 >= 10000 && sharedUid1 <= 19999, "Shared UID must be in application UID range");
            check(sharedUid1 == sharedUid2, "Packages with identical sharedUserId must share the same UID");
            check(sharedUid1 != uid1 && sharedUid1 != uid2 && sharedUid1 != uid3,
                    "Shared UID must be distinct from standard package UIDs");
            check(PackageUids.uidFor("com.example.shared.a") == sharedUid1, "Shared app a allocation must be stable");
            check(PackageUids.uidFor("com.example.shared.b") == sharedUid2, "Shared app b allocation must be stable");

            // Test 4: Second distinct shared group gets a different shared UID
            String otherGroup = "com.example.other.group";
            int otherSharedUid = admitShared("com.example.other.c", otherGroup);
            check(otherSharedUid != sharedUid1, "Different sharedUserId groups must receive distinct UIDs");

            // Test 5: Durability across registry reopening
            File registryFile = new File(tempDir, "system/package-uids");
            check(registryFile.isFile(), "package-uids registry file must exist");
            Properties persisted = new Properties();
            try (FileInputStream in = new FileInputStream(registryFile)) { persisted.load(in); }
            check(Integer.toString(uid1).equals(persisted.getProperty("com.example.app1")),
                    "app1 must be persisted in package-uids");
            check(Integer.toString(sharedUid1).equals(persisted.getProperty("com.example.shared.a")),
                    "shared app a must be persisted in package-uids");
            check(Integer.toString(sharedUid1).equals(persisted.getProperty("com.example.shared.b")),
                    "shared app b must be persisted in package-uids");
            check(Integer.toString(sharedUid1).equals(persisted.getProperty("shared:" + sharedGroup)),
                    "shared group mapping must be persisted");

            // Test 6: PackageManagerService integration: getPackageUid & getPackagesForUid
            String perm = "android.permission.ACCESS_FINE_LOCATION";
            ApkPackage pkg1 = createPackage("com.example.app1", uid1, null, perm);
            ApkPackage pkg2 = createPackage("com.example.app2", uid2, null, perm);
            ApkPackage pkgSharedA = createPackage("com.example.shared.a", sharedUid1, sharedGroup, perm);
            ApkPackage pkgSharedB = createPackage("com.example.shared.b", sharedUid1, sharedGroup, perm);

            List<ApkPackage> allPackages = Arrays.asList(pkg1, pkg2, pkgSharedA, pkgSharedB);
            PackageManagerService pms = new PackageManagerService(allPackages);

            // getPackageUid tests
            check(pms.getPackageUid("com.example.app1", 0, 0) == uid1, "getPackageUid for app1 must return uid1");
            check(pms.getPackageUid("com.example.app2", 0, 0) == uid2, "getPackageUid for app2 must return uid2");
            check(pms.getPackageUid("com.example.shared.a", 0, 0) == sharedUid1,
                    "getPackageUid for shared app a must return sharedUid1");
            check(pms.getPackageUid("com.example.shared.b", 0, 0) == sharedUid1,
                    "getPackageUid for shared app b must return sharedUid1");
            check(pms.getPackageUid("com.nonexistent.package", 0, 0) == -1,
                    "getPackageUid for nonexistent package must return -1");

            // getPackagesForUid tests
            String[] pkgsForUid1 = pms.getPackagesForUid(uid1);
            check(pkgsForUid1 != null && pkgsForUid1.length == 1 && "com.example.app1".equals(pkgsForUid1[0]),
                    "getPackagesForUid for uid1 must return only app1");

            String[] pkgsForUid2 = pms.getPackagesForUid(uid2);
            check(pkgsForUid2 != null && pkgsForUid2.length == 1 && "com.example.app2".equals(pkgsForUid2[0]),
                    "getPackagesForUid for uid2 must return only app2");

            String[] pkgsForShared = pms.getPackagesForUid(sharedUid1);
            check(pkgsForShared != null && pkgsForShared.length == 2,
                    "getPackagesForUid for shared UID must return both packages");
            Set<String> sharedSet = new HashSet<>(Arrays.asList(pkgsForShared));
            check(sharedSet.contains("com.example.shared.a") && sharedSet.contains("com.example.shared.b"),
                    "getPackagesForUid for shared UID must contain both shared.a and shared.b");

            check(pms.getPackagesForUid(19998) == null,
                    "getPackagesForUid for unused UID must return null");

            // Test 7: System packages retain system UIDs
            check(pms.getPackageUid("android", 0, 0) == Process.SYSTEM_UID,
                    "Platform package 'android' must retain SYSTEM_UID (1000)");
            String[] systemPkgs = pms.getPackagesForUid(Process.SYSTEM_UID);
            check(systemPkgs != null && systemPkgs.length == 1 && "android".equals(systemPkgs[0]),
                    "getPackagesForUid(SYSTEM_UID) must return 'android'");
            check("android.uid.system:1000".equals(pms.getNameForUid(Process.SYSTEM_UID)),
                    "getNameForUid(SYSTEM_UID) must return 'android.uid.system:1000'");

            check(pms.getPackageUid("com.android.shell", 0, 0) == Process.SHELL_UID,
                    "Shell package 'com.android.shell' must retain SHELL_UID (2000)");
            String[] shellPkgs = pms.getPackagesForUid(Process.SHELL_UID);
            check(shellPkgs != null && shellPkgs.length == 1 && "com.android.shell".equals(shellPkgs[0]),
                    "getPackagesForUid(SHELL_UID) must return 'com.android.shell'");
            check("android.uid.shell:2000".equals(pms.getNameForUid(Process.SHELL_UID)),
                    "getNameForUid(SHELL_UID) must return 'android.uid.shell:2000'");
            check("shared:android.uid.shell".equals(
                            pms.getNamesForUids(new int[] {Process.SHELL_UID})[0]),
                    "The bulk shell identity must use the shared: prefix");

            // Test 8: getNameForUid for single vs shared UID
            check("com.example.app1".equals(pms.getNameForUid(uid1)),
                    "getNameForUid for single app must return package name");
            check((sharedGroup + ":" + sharedUid1).equals(pms.getNameForUid(sharedUid1)),
                    "getNameForUid for shared UID must return sharedUserId:appId");

            String[] namesForUids = pms.getNamesForUids(new int[] {Process.SYSTEM_UID, uid1, sharedUid1});
            check(namesForUids != null && namesForUids.length == 3, "getNamesForUids must return 3 results");
            check("shared:android.uid.system".equals(namesForUids[0]), "Positional system UID must be shared:android.uid.system");
            check("com.example.app1".equals(namesForUids[1]), "Positional app1 name must match");
            check(("shared:" + sharedGroup).equals(namesForUids[2]),
                    "Positional shared UID name must use the shared: prefix");

            PackageManagerService oneSharedMember = new PackageManagerService(List.of(pkgSharedA));
            check((sharedGroup + ":" + sharedUid1).equals(oneSharedMember.getNameForUid(sharedUid1)),
                    "A shared UID retains its identity with only one installed member");
            check(("shared:" + sharedGroup).equals(
                            oneSharedMember.getNamesForUids(new int[] {sharedUid1})[0]),
                    "A one-member shared UID retains the bulk shared: prefix");
            check(pms.getNameForUid(19998) == null,
                    "An unknown UID has no singular name");
            check(pms.getNamesForUids(new int[] {19998, uid1})[0] == null,
                    "An unknown UID retains its null position in bulk names");

            // Test 9: Permission isolation across distinct UIDs and sharing within sharedUserId

            PermissionPolicy policy = pms.permissions();
            if (!art) {
                // Host-only metadata boundary: no Android resources are installed in
                // the JVM. Runtime permission decisions execute the production policy.
                Field catalogField = PermissionPolicy.class.getDeclaredField("mCatalog");
                catalogField.setAccessible(true); Object catalog = catalogField.get(policy);
                Field loaded = catalog.getClass().getDeclaredField("mLoaded"); loaded.setAccessible(true); loaded.set(catalog, true);
                Field definitions = catalog.getClass().getDeclaredField("mFramework"); definitions.setAccessible(true);
                PermissionInfo definition = new PermissionInfo(); definition.name = perm; definition.packageName = "android";
                definition.protectionLevel = PermissionInfo.PROTECTION_DANGEROUS;
                ((java.util.Map<String, PermissionInfo>) definitions.get(catalog)).put(perm, definition);
            }
            // Initially, permissions are not granted
            check(policy.checkUidPermission(uid1, perm, 0) == PackageManager.PERMISSION_DENIED,
                    "uid1 must initially have permission denied");
            check(policy.checkUidPermission(uid2, perm, 0) == PackageManager.PERMISSION_DENIED,
                    "uid2 must initially have permission denied");
            check(policy.checkUidPermission(sharedUid1, perm, 0) == PackageManager.PERMISSION_DENIED,
                    "sharedUid1 must initially have permission denied");

            // Grant permission to shared app a
            policy.grantRuntime("com.example.shared.a", perm, 0);

            // Both shared packages and their UID must now be granted
            check(policy.checkUidPermission(sharedUid1, perm, 0) == PackageManager.PERMISSION_GRANTED,
                    "sharedUid1 must have permission granted after grant to shared.a");
            check(policy.checkPermission("com.example.shared.a", perm) == PackageManager.PERMISSION_GRANTED,
                    "shared.a must have permission granted");
            check(policy.checkPermission("com.example.shared.b", perm) == PackageManager.PERMISSION_GRANTED,
                    "shared.b must inherit permission granted to shared.a under same sharedUserId");

            // Standard packages uid1 and uid2 MUST REMAIN DENIED (isolation!)
            check(policy.checkUidPermission(uid1, perm, 0) == PackageManager.PERMISSION_DENIED,
                    "uid1 MUST REMAIN DENIED when permission was granted to shared group (isolation)");
            check(policy.checkUidPermission(uid2, perm, 0) == PackageManager.PERMISSION_DENIED,
                    "uid2 MUST REMAIN DENIED when permission was granted to shared group (isolation)");
            check(policy.checkPermission("com.example.app1", perm) == PackageManager.PERMISSION_DENIED,
                    "app1 must remain denied (isolation)");
            check(policy.checkPermission("com.example.app2", perm) == PackageManager.PERMISSION_DENIED,
                    "app2 must remain denied (isolation)");

            // Grant permission to app1
            policy.grantRuntime("com.example.app1", perm, 0);
            check(policy.checkUidPermission(uid1, perm, 0) == PackageManager.PERMISSION_GRANTED,
                    "uid1 must be granted after grant to app1");
            check(policy.checkPermission("com.example.app1", perm) == PackageManager.PERMISSION_GRANTED,
                    "app1 must be granted");
            // app2 MUST STILL BE DENIED (isolation between app1 and app2!)
            check(policy.checkUidPermission(uid2, perm, 0) == PackageManager.PERMISSION_DENIED,
                    "uid2 MUST REMAIN DENIED when permission was granted to app1 (isolation)");
            check(policy.checkPermission("com.example.app2", perm) == PackageManager.PERMISSION_DENIED,
                    "app2 must remain denied when app1 was granted (isolation)");

            // An authenticated package in the same UID sees the shared permission
            // even when only its sibling requested it in the manifest.
            ApkPackage sibling = createPackage("com.example.shared.sibling", sharedUid1, sharedGroup);
            var add = PackageManagerService.class.getDeclaredMethod("add", ApkPackage.class); add.setAccessible(true);
            add.invoke(pms, sibling);
            check(policy.checkPermission(sibling.packageName, perm) == PackageManager.PERMISSION_GRANTED,
                    "Package and UID permission queries must use the same shared state");
            policy.revokeRuntimePermission(pkgSharedA.packageName, perm, "default:0", 0, "fixture revoke");
            check(policy.checkPermission(pkgSharedB.packageName, perm) == PackageManager.PERMISSION_DENIED,
                    "Revocation must reach every shared member");
            check(policy.checkPermission(sibling.packageName, perm) == PackageManager.PERMISSION_DENIED,
                    "A sibling cannot retain a stale per-package grant");
            policy.grantRuntime(pkgSharedA.packageName, perm, 0);
            policy.forgetPackage(pkgSharedA.packageName);
            check(policy.checkPermission(pkgSharedB.packageName, perm) == PackageManager.PERMISSION_GRANTED,
                    "Removing one requester must retain a grant still used by a shared sibling");

            Properties admitted = registry(tempDir);
            denied(() -> PackageUids.uidFor("com.example.unadmitted", sharedGroup),
                    "A name and sharedUserId must not allocate a shared identity");
            check(registry(tempDir).equals(admitted), "Rejected lookup must not change persistent membership");
            Signature[] unrelated = ApkPackage.verifySignatures(new File(new File(signedFixture).getParentFile().getParentFile(),
                    "identity-fixtures/old.apk").getPath());
            check(unrelated != null && !Arrays.equals(unrelated, verifiedFixture), "Independent signer fixture must verify");
            ApkPackage incompatible = pending("com.example.incompatible", sharedGroup, unrelated);
            denied(() -> new PackageManagerService(List.of(incompatible)), "Boot/direct admission must verify shared signers");
            check(incompatible.uid() == -1 && registry(tempDir).equals(admitted),
                    "Rejected admission must not assign a UID or persist membership");
            ApkPackage changedGroup = pending(pkgSharedA.packageName, "com.example.changed.group", verifiedFixture);
            denied(() -> new PackageManagerService(List.of(changedGroup)), "An active package cannot change shared identity");
            check(changedGroup.uid() == -1 && registry(tempDir).equals(admitted),
                    "Rejected shared-ID change must preserve the prior registry");
            ApkPackage tentative = pending("com.example.tentative", null, verifiedFixture);
            Object admission = prepare(tentative);
            transaction(admission, "commit");
            check(tentative.uid() >= 10000, "Prepared publication receives its provisional admitted UID");
            transaction(admission, "close");
            check(tentative.uid() == -1 && registry(tempDir).equals(admitted),
                    "Failed publication rolls back the UID and all identity metadata");

            // Verified APK lineage, actual PM publication and retirement exercise
            // ordinary signed updates, rather than only comparing certificate arrays.
            File lineageFixtures = new File(new File(signedFixture).getParentFile().getParentFile(),
                    "identity-fixtures");
            String rotationGroup = "com.example.rotation.group";
            ApkPackage beforeRotation = signedPending("com.example.rotation.member", rotationGroup,
                    new File(lineageFixtures, "old.apk"));
            ApkPackage rotationPeer = signedPending("com.example.rotation.peer", rotationGroup,
                    new File(lineageFixtures, "old.apk"));
            PackageManagerService rotationPm = new PackageManagerService(List.of(beforeRotation, rotationPeer));
            int rotationUid = beforeRotation.uid();
            check(rotationPeer.uid() == rotationUid, "Verified shared peers must receive the same admitted UID");
            ApkPackage rotated = signedPending(beforeRotation.packageName, rotationGroup,
                    new File(lineageFixtures, "rotated-no-auth.apk"));
            add.invoke(rotationPm, rotated);
            check(rotated.uid() == rotationUid && rotationPeer.uid() == rotationUid,
                    "A verified rotation with installed-data and shared-UID capability retains the shared UID");
            check(rotationPm.getPackageUid(rotated.packageName, 0, 0) == rotationUid,
                    "PM publication must expose the rotated package under the admitted UID");

            var retireMethod = PackageUids.class.getDeclaredMethod("prepareRetire", ApkPackage.class);
            retireMethod.setAccessible(true);
            Properties beforeRetire = registry(tempDir);
            Object retirement = retireMethod.invoke(null, rotated);
            transaction(retirement, "commit");
            transaction(retirement, "close");
            check(registry(tempDir).equals(beforeRetire),
                    "An unaccepted retirement restores the complete prior identity snapshot");

            var remove = PackageManagerService.class.getDeclaredMethod("remove", String.class);
            remove.setAccessible(true); remove.invoke(rotationPm, rotated.packageName);
            check(rotationPm.getPackageUid(rotated.packageName, 0, 0) == -1,
                    "A retired package is absent from package queries");
            check(!registry(tempDir).containsKey("active:" + rotated.packageName),
                    "Accepted retirement clears active signer ownership");
            ApkPackage reinstalled = signedPending(rotated.packageName, "com.example.rotation.newgroup",
                    new File(lineageFixtures, "rotated-no-auth.apk"));
            add.invoke(rotationPm, reinstalled);
            check(reinstalled.uid() != rotationUid && rotationPeer.uid() == rotationUid,
                    "Reinstalling into a new shared identity allocates a distinct UID and preserves the old peer");

            // Test 10: Registry corruption detection
            Properties corrupt = new Properties();
            try (FileInputStream in = new FileInputStream(registryFile)) { corrupt.load(in); }
            // Add a duplicate non-shared package entry with same UID as app1
            corrupt.setProperty("com.example.duplicate", Integer.toString(uid1));
            try (FileOutputStream out = new FileOutputStream(registryFile)) { corrupt.store(out, "corrupt"); }

            boolean rejected = false;
            try {
                PackageUids.uidFor("com.example.should.fail");
            } catch (IllegalStateException expected) {
                rejected = (expected.getMessage() != null && expected.getMessage().contains("Corrupt package UID registry"))
                        || (expected.getCause() != null && expected.getCause().getMessage() != null && expected.getCause().getMessage().contains("Corrupt package UID registry"));
            }
            check(rejected, "Corrupt UID registry with conflicting non-shared UIDs must fail closed");

            System.out.println("PackageUidIsolationContractTest passed: " + checks + " checks");
        } finally {
            deleteRecursively(tempDir);
        }
    }

    /** Isolated child-process phases exercise interruption of the real transaction. */
    public static void transactionPhase(String signedFixture, String phase) throws Exception {
        Field access = Unsafe.class.getDeclaredField("theUnsafe"); access.setAccessible(true);
        unsafe = (Unsafe) access.get(null);
        verifiedFixture = ApkPackage.verifySignatures(signedFixture);
        File data = new File(System.getenv("ANDROID_DATA")); Files.createDirectories(data.toPath());
        String name = "com.example.interrupted";
        if (phase.startsWith("prepare-")) {
            File code = new File(data, "app/interrupted-1"); Files.createDirectories(code.toPath());
            File base = new File(code, "base.apk"); Files.copy(new File(signedFixture).toPath(), base.toPath());
            ApkPackage pkg = pending(name, null, verifiedFixture); set(pkg, "apkPath", base.getCanonicalPath());
            Object admission = prepare(pkg); transaction(admission, "commit");
            if (phase.equals("prepare-published")) {
                Files.writeString(new File(data, "system/packages.list").toPath(),
                        name + "\t" + code.getCanonicalPath() + "\t1\t1\n");
            }
            check(new File(data, "system/package-uids.pending").isFile(), "Transaction recovery evidence must exist");
            System.out.println("UID interruption fixture prepared: " + phase);
            // Deliberately end this isolated process without accepting or closing the
            // transaction. OS handle cleanup releases its file lock, as after a crash.
            return;
        }
        check(new File(data, "system/package-uids.pending").isFile(), "Interrupted transaction must reach recovery");
        int next = PackageUids.uidFor("com.example.afterinterruption");
        Properties recovered = registry(data);
        boolean published = phase.equals("check-published");
        check(recovered.containsKey(name) == published, "Recovery must follow durable package publication");
        check(next == (published ? 10001 : 10000), "Recovered allocator must retain only committed identities");
        check(!new File(data, "system/package-uids.pending").exists(), "Completed recovery must retire the journal");
        System.out.println("UID interruption recovery passed: " + phase);
    }

    private static void deleteRecursively(File file) {
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) {
                for (File child : children) deleteRecursively(child);
            }
        }
        file.delete();
    }
}
