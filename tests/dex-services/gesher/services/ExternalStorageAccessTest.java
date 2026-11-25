/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.app.AppOpsManager;
import android.os.IBinder;
import android.os.Process;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.Device;
import com.gesher.server.ExternalStorageAccess;
import com.gesher.server.PackageManagerService;
import com.gesher.server.RuntimeCommands;
import com.gesher.server.Services;
import com.gesher.server.SharedLibraryGraph;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.PrintStream;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;
import java.util.Map;
import java.util.Set;
import sun.misc.Unsafe;

/**
 * Tests for {@link ExternalStorageAccess}, policy persistence, sync hooks,
 * {@link Services.StorageService#mkdirs}, and {@code RuntimeCommands} appops.
 */
public final class ExternalStorageAccessTest {
    private static int checks;

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }

    private static Unsafe unsafe() throws Exception {
        Field f = Unsafe.class.getDeclaredField("theUnsafe");
        f.setAccessible(true);
        return (Unsafe) f.get(null);
    }

    private static void set(Object obj, String fieldName, Object value) throws Exception {
        Field f = obj.getClass().getDeclaredField(fieldName);
        f.setAccessible(true);
        f.set(obj, value);
    }

    private static PackageManagerService createPm(List<ApkPackage> packages) throws Exception {
        Map<String, IBinder> created = Services.createInProcess(packages);
        Services.installInProcess(created);
        Field sLast = Services.class.getDeclaredField("sLast");
        sLast.setAccessible(true);
        Object last = sLast.get(null);
        Field pmField = last.getClass().getDeclaredField("pm");
        pmField.setAccessible(true);
        return (PackageManagerService) pmField.get(last);
    }

    private static ApkPackage makePackage(Unsafe unsafe, String name, int uid, int targetSdk,
            boolean legacyFlag, String... permissions) throws Exception {
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(name, "/data/app/" + name + "/base.apk");
        parsed.setTargetSdkVersion(targetSdk);
        parsed.setEnabled(true);
        parsed.setRequestLegacyExternalStorage(legacyFlag);
        for (String perm : permissions) {
            parsed.addUsesPermission(new ParsedUsesPermissionImpl(perm, 0, Set.of()));
        }
        ApkPackage pkg = (ApkPackage) unsafe.allocateInstance(ApkPackage.class);
        set(pkg, "packageName", name);
        set(pkg, "parsed", parsed);
        set(pkg, "apkPath", "/data/app/" + name + "/base.apk");
        set(pkg, "mUid", uid);
        set(pkg, "sharedLibraries", SharedLibraryGraph.EMPTY);
        pkg.setDataDir(new File(Device.dataDir(), "data/" + name).getPath());
        return pkg;
    }

    private static void testDeclaresPermission(Unsafe unsafe) throws Exception {
        ApkPackage withPerm = makePackage(unsafe, "com.test.withperm", 10100, 33, false,
                Manifest.permission.MANAGE_EXTERNAL_STORAGE);
        ApkPackage withoutPerm = makePackage(unsafe, "com.test.withoutperm", 10101, 33, false,
                Manifest.permission.READ_EXTERNAL_STORAGE);
        check(!ExternalStorageAccess.declaresPermission(null), "null package declares permission");
        check(ExternalStorageAccess.declaresPermission(withPerm), "package with MANAGE_EXTERNAL_STORAGE");
        check(!ExternalStorageAccess.declaresPermission(withoutPerm), "package without MANAGE_EXTERNAL_STORAGE");
    }

    private static void testModesAndPersistence(Unsafe unsafe) throws Exception {
        ApkPackage corePkg = makePackage(unsafe, "com.test.core", Process.SYSTEM_UID, 34, false);
        ApkPackage mgrPkg = makePackage(unsafe, "com.test.mgr", 10200, 34, false,
                Manifest.permission.MANAGE_EXTERNAL_STORAGE);
        ApkPackage legacy28 = makePackage(unsafe, "com.test.legacy28", 10201, 28, false,
                Manifest.permission.READ_EXTERNAL_STORAGE,
                Manifest.permission.WRITE_EXTERNAL_STORAGE);
        ApkPackage legacy29 = makePackage(unsafe, "com.test.legacy29", 10202, 29, true,
                Manifest.permission.READ_EXTERNAL_STORAGE);
        ApkPackage media32 = makePackage(unsafe, "com.test.media32", 10203, 32, false,
                Manifest.permission.READ_EXTERNAL_STORAGE);
        ApkPackage media33 = makePackage(unsafe, "com.test.media33", 10204, 33, false,
                Manifest.permission.READ_MEDIA_IMAGES,
                Manifest.permission.READ_MEDIA_AUDIO);
        ApkPackage scoped34 = makePackage(unsafe, "com.test.scoped34", 10205, 34, false,
                Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED);

        PackageManagerService pm = createPm(
                List.of(corePkg, mgrPkg, legacy28, legacy29, media32, media33, scoped34));

        // Core UID is manager by default
        check(ExternalStorageAccess.isExternalStorageManager(pm, corePkg), "core uid should be manager");

        // Manager app: MODE_DEFAULT -> false, MODE_ALLOWED -> true, MODE_IGNORED -> false
        check(!ExternalStorageAccess.isExternalStorageManager(pm, mgrPkg), "mgr default should be false");
        check(ExternalStorageAccess.computePolicy(pm, mgrPkg).mode() == ExternalStorageAccess.Mode.SCOPED,
                "mgr default should be SCOPED");

        pm.appOps().setUidMode(AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE, mgrPkg.uid(), AppOpsManager.MODE_ALLOWED);
        check(ExternalStorageAccess.isExternalStorageManager(pm, mgrPkg), "mgr allowed should be true");
        ExternalStorageAccess.Policy mgrPolicy = ExternalStorageAccess.computePolicy(pm, mgrPkg);
        check(mgrPolicy.mode() == ExternalStorageAccess.Mode.MANAGER && mgrPolicy.mediaMask() == 7,
                "mgr allowed policy should be MANAGER/7");

        // Verify policy file written by AppOpsService hook
        File mgrFile = ExternalStorageAccess.policyFile(mgrPkg.packageName);
        if (mgrFile != null) {
            String content = Files.readString(mgrFile.toPath());
            check(content.equals("v=1\npkg=com.test.mgr\nuid=10200\ndeclared=1\nmode=MANAGER\nmedia_mask=7\n"),
                    "unexpected policy file content: " + content);
        }

        // Legacy 28 with WRITE_EXTERNAL_STORAGE -> LEGACY_RW
        pm.permissions().grantRuntimePermission(legacy28.packageName,
                Manifest.permission.WRITE_EXTERNAL_STORAGE, "default:0", 0);
        ExternalStorageAccess.Policy l28Policy = ExternalStorageAccess.computePolicy(pm, legacy28);
        check(l28Policy.mode() == ExternalStorageAccess.Mode.LEGACY_RW && l28Policy.mediaMask() == 7,
                "legacy28 should be LEGACY_RW/7, got " + l28Policy);

        // Legacy 29 with requestLegacyExternalStorage=true and READ_EXTERNAL_STORAGE -> LEGACY_RO
        pm.permissions().grantRuntimePermission(legacy29.packageName,
                Manifest.permission.READ_EXTERNAL_STORAGE, "default:0", 0);
        ExternalStorageAccess.Policy l29Policy = ExternalStorageAccess.computePolicy(pm, legacy29);
        check(l29Policy.mode() == ExternalStorageAccess.Mode.LEGACY_RO && l29Policy.mediaMask() == 7,
                "legacy29 should be LEGACY_RO/7, got " + l29Policy);

        // API 32 with READ_EXTERNAL_STORAGE -> MEDIA_RO/7
        pm.permissions().grantRuntimePermission(media32.packageName,
                Manifest.permission.READ_EXTERNAL_STORAGE, "default:0", 0);
        ExternalStorageAccess.Policy m32Policy = ExternalStorageAccess.computePolicy(pm, media32);
        check(m32Policy.mode() == ExternalStorageAccess.Mode.MEDIA_RO && m32Policy.mediaMask() == 7,
                "media32 should be MEDIA_RO/7, got " + m32Policy);

        // API 33 with READ_MEDIA_IMAGES + READ_MEDIA_AUDIO -> MEDIA_RO/5
        pm.permissions().grantRuntimePermission(media33.packageName,
                Manifest.permission.READ_MEDIA_IMAGES, "default:0", 0);
        pm.permissions().grantRuntimePermission(media33.packageName,
                Manifest.permission.READ_MEDIA_AUDIO, "default:0", 0);
        ExternalStorageAccess.Policy m33Policy = ExternalStorageAccess.computePolicy(pm, media33);
        check(m33Policy.mode() == ExternalStorageAccess.Mode.MEDIA_RO && m33Policy.mediaMask() == 5,
                "media33 should be MEDIA_RO/5, got " + m33Policy);

        // API 34 with READ_MEDIA_VISUAL_USER_SELECTED -> MEDIA_RO/3
        pm.permissions().grantRuntimePermission(scoped34.packageName,
                Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED, "default:0", 0);
        ExternalStorageAccess.Policy s34Policy = ExternalStorageAccess.computePolicy(pm, scoped34);
        check(s34Policy.mode() == ExternalStorageAccess.Mode.MEDIA_RO && s34Policy.mediaMask() == 3,
                "scoped34 should be MEDIA_RO/3, got " + s34Policy);

        // Revoke returns to SCOPED/0
        pm.permissions().revokeRuntimePermission(scoped34.packageName,
                Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED, "default:0", 0, "test");
        ExternalStorageAccess.Policy s34Revoked = ExternalStorageAccess.computePolicy(pm, scoped34);
        check(s34Revoked.mode() == ExternalStorageAccess.Mode.SCOPED && s34Revoked.mediaMask() == 0,
                "revoked scoped34 should be SCOPED/0, got " + s34Revoked);

        // RuntimeCommands appops get/set
        Method appops = RuntimeCommands.class.getDeclaredMethod("appops", String[].class);
        appops.setAccessible(true);
        check(runAppops(appops, "get", mgrPkg.packageName, "MANAGE_EXTERNAL_STORAGE")
                .equals("declared=1 mode=allow allowed=1"), "runtime appops get allow");
        runAppops(appops, "set", mgrPkg.packageName, "MANAGE_EXTERNAL_STORAGE", "ignore");
        check(runAppops(appops, "get", mgrPkg.packageName, "MANAGE_EXTERNAL_STORAGE")
                .equals("declared=1 mode=ignore allowed=0"), "runtime appops get ignore");
        runAppops(appops, "set", mgrPkg.packageName, "MANAGE_EXTERNAL_STORAGE", "default");
        check(runAppops(appops, "get", mgrPkg.packageName, "MANAGE_EXTERNAL_STORAGE")
                .equals("declared=1 mode=default allowed=0"), "runtime appops get default");

        // syncAll, resetAllModes & removePackage
        ExternalStorageAccess.syncAll(pm);
        if (mgrFile != null) check(mgrFile.isFile(), "syncAll should write policy file");
        pm.appOps().setUidMode(AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE, mgrPkg.uid(), AppOpsManager.MODE_ALLOWED);
        pm.appOps().resetAllModes(0, mgrPkg.packageName);
        check(!ExternalStorageAccess.isExternalStorageManager(pm, mgrPkg), "resetAllModes should reset manager");
        if (mgrFile != null) {
            String resetContent = Files.readString(mgrFile.toPath());
            check(resetContent.contains("mode=SCOPED\n"), "resetAllModes should sync policy file to SCOPED");
        }
        ExternalStorageAccess.removePackage(mgrPkg.packageName);
        if (mgrFile != null) check(!mgrFile.exists(), "removePackage should delete policy file");
    }

    private static String runAppops(Method appops, String... tail) throws Exception {
        String[] args = new String[tail.length + 1];
        args[0] = "appops";
        System.arraycopy(tail, 0, args, 1, tail.length);
        PrintStream saved = System.out;
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        System.setOut(new PrintStream(out, true, StandardCharsets.UTF_8));
        try {
            int rc = (int) appops.invoke(null, (Object) args);
            check(rc == 0, "appops command exited with " + rc);
        } finally {
            System.setOut(saved);
        }
        return out.toString(StandardCharsets.UTF_8).trim();
    }

    private static void testPathAccessAndMkdirs(Unsafe unsafe) throws Exception {
        ApkPackage mgrPkg = makePackage(unsafe, "com.test.pathmgr", 10300, 34, false,
                Manifest.permission.MANAGE_EXTERNAL_STORAGE);
        ApkPackage scopedPkg = makePackage(unsafe, "com.test.pathscoped", 10301, 34, false);
        ApkPackage roPkg = makePackage(unsafe, "com.test.pathro", 10302, 29, true,
                Manifest.permission.READ_EXTERNAL_STORAGE);
        PackageManagerService pm = createPm(List.of(mgrPkg, scopedPkg, roPkg));
        pm.appOps().setUidMode(AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE, mgrPkg.uid(), AppOpsManager.MODE_ALLOWED);
        pm.permissions().grantRuntimePermission(roPkg.packageName,
                Manifest.permission.READ_EXTERNAL_STORAGE, "default:0", 0);

        String sharedRoot = new File(Device.dataDir(), "media/0").getPath();
        // Core UID always allowed
        check(ExternalStorageAccess.canAccessPath(pm, Process.SYSTEM_UID, "android",
                sharedRoot + "/Android/data/com.test.pathscoped/files", true), "core allowed");

        // Own Android/{data,obb,media}/<pkg> allowed even for SCOPED
        for (String sub : new String[] {"data", "obb", "media"}) {
            check(ExternalStorageAccess.canAccessPath(pm, scopedPkg.uid(), scopedPkg.packageName,
                    sharedRoot + "/Android/" + sub + "/" + scopedPkg.packageName + "/files", true),
                    "own Android/" + sub + " allowed for scoped");
        }

        // Other Android/data or Android/obb denied even for MANAGER
        check(!ExternalStorageAccess.canAccessPath(pm, mgrPkg.uid(), mgrPkg.packageName,
                sharedRoot + "/Android/data/" + scopedPkg.packageName + "/files", true),
                "other Android/data denied for manager");
        check(!ExternalStorageAccess.canAccessPath(pm, mgrPkg.uid(), mgrPkg.packageName,
                sharedRoot + "/Android/obb/" + scopedPkg.packageName, false),
                "other Android/obb denied for manager");

        // Shared storage write: MANAGER allowed, LEGACY_RO and SCOPED denied
        check(ExternalStorageAccess.canAccessPath(pm, mgrPkg.uid(), mgrPkg.packageName,
                sharedRoot + "/Download/appdir", true), "manager can write shared");
        check(ExternalStorageAccess.canAccessPath(pm, mgrPkg.uid(), mgrPkg.packageName,
                "/sdcard/Download/appdir", true), "manager can write /sdcard shared");
        check(!ExternalStorageAccess.canAccessPath(pm, mgrPkg.uid(), mgrPkg.packageName,
                "/sdcard/../../escape", true), "traversal above /sdcard denied");
        check(!ExternalStorageAccess.canAccessPath(pm, roPkg.uid(), roPkg.packageName,
                sharedRoot + "/Download/appdir", true), "legacy_ro cannot write shared");
        check(ExternalStorageAccess.canAccessPath(pm, roPkg.uid(), roPkg.packageName,
                sharedRoot + "/Download/appdir", false), "legacy_ro can read shared");
        check(!ExternalStorageAccess.canAccessPath(pm, scopedPkg.uid(), scopedPkg.packageName,
                sharedRoot + "/Download/appdir", true), "scoped cannot write shared");

        // MEDIA_RO: media read allowed, non-media read denied
        ApkPackage mediaPkg = makePackage(unsafe, "com.test.pathmedia", 10303, 33, false,
                Manifest.permission.READ_MEDIA_IMAGES);
        PackageManagerService mediaPm = createPm(List.of(mediaPkg));
        mediaPm.permissions().grantRuntimePermission(mediaPkg.packageName,
                Manifest.permission.READ_MEDIA_IMAGES, "default:0", 0);
        check(ExternalStorageAccess.canAccessPath(mediaPm, mediaPkg.uid(), mediaPkg.packageName,
                sharedRoot + "/Pictures/photo.jpg", false), "MEDIA_RO can read image");
        check(!ExternalStorageAccess.canAccessPath(mediaPm, mediaPkg.uid(), mediaPkg.packageName,
                sharedRoot + "/Download/notes.txt", false), "MEDIA_RO cannot read txt");

        // StorageService.mkdirs works for system caller
        Services.StorageService storage = new Services.StorageService();
        File testDir = new File(sharedRoot, "Android/data/" + scopedPkg.packageName + "/test-mkdirs");
        storage.mkdirs("android", testDir.getPath());
        check(testDir.isDirectory(), "StorageService.mkdirs should create directory");
    }

    public static void main(String[] args) throws Exception {
        Unsafe unsafe = unsafe();
        testDeclaresPermission(unsafe);
        testModesAndPersistence(unsafe);
        testPathAccessAndMkdirs(unsafe);
        System.out.println("ExternalStorageAccessTest passed (" + checks + " checks)");
    }
}
