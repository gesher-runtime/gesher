/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import com.gesher.server.ApkPackage;
import com.gesher.server.MicrogAddons;
import java.io.File;
import java.io.IOException;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.FileTime;
import java.util.Arrays;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.stream.Stream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import sun.misc.Unsafe;

/** Tests idempotent native-library extraction and stale cleanup for staged microG addons. */
public final class MicrogAddonNativeLibsTest {
    private static int checks;

    private MicrogAddonNativeLibsTest() {
    }

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }

    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name);
        f.setAccessible(true);
        return f;
    }

    private static Method method(Class<?> type, String name, Class<?>... params) throws Exception {
        Method m = type.getDeclaredMethod(name, params);
        m.setAccessible(true);
        return m;
    }

    private static Object invoke(Method m, Object target, Object... args) throws Exception {
        try {
            return m.invoke(target, args);
        } catch (InvocationTargetException failure) {
            Throwable cause = failure.getCause();
            if (cause instanceof Exception) throw (Exception) cause;
            if (cause instanceof Error) throw (Error) cause;
            throw failure;
        }
    }

    private static ApkPackage allocateApkPackage(Unsafe unsafe, String apkPath) throws Exception {
        ApkPackage pkg = (ApkPackage) unsafe.allocateInstance(ApkPackage.class);
        field(ApkPackage.class, "apkPath").set(pkg, apkPath);
        return pkg;
    }

    private static void writeZip(Path target, Map<String, byte[]> entries) throws IOException {
        Files.createDirectories(target.getParent());
        try (ZipOutputStream out = new ZipOutputStream(Files.newOutputStream(target))) {
            for (Map.Entry<String, byte[]> e : entries.entrySet()) {
                out.putNextEntry(new ZipEntry(e.getKey()));
                out.write(e.getValue());
                out.closeEntry();
            }
        }
    }

    private static byte[] bytes(String text) {
        return text.getBytes(StandardCharsets.UTF_8);
    }

    private static void deleteTree(Path root) throws IOException {
        if (!Files.exists(root)) return;
        try (Stream<Path> walk = Files.walk(root)) {
            for (Path p : walk.sorted(Comparator.reverseOrder()).toArray(Path[]::new)) {
                Files.deleteIfExists(p);
            }
        }
    }

    public static void main(String[] args) throws Exception {
        Field unsafeField = Unsafe.class.getDeclaredField("theUnsafe");
        unsafeField.setAccessible(true);
        Unsafe unsafe = (Unsafe) unsafeField.get(null);

        Method prepareNativeLibraries = method(MicrogAddons.class, "prepareNativeLibraries",
                ApkPackage.class);
        Method syncNativeLibraries = method(MicrogAddons.class, "syncNativeLibraries",
                File.class, File.class, String.class);
        Field primaryCpuAbiField = field(ApkPackage.class, "mPrimaryCpuAbi");

        Path root = Files.createTempDirectory("gesher-microg-native-libs-");
        try {
            // 1. Multi-ABI APK: prefers x86_64 over arm64-v8a and extracts x86_64 payload.
            Path gmsDir = root.resolve("priv-app/com.google.android.gms");
            Path gmsApk = gmsDir.resolve("base.apk");
            byte[] x64Conscrypt = bytes("x64-conscrypt-payload-v1");
            byte[] x64Cronet = bytes("x64-cronet-payload-v1");
            Map<String, byte[]> multiAbi = new LinkedHashMap<>();
            multiAbi.put("classes.dex", bytes("dex"));
            multiAbi.put("lib/arm64-v8a/libconscrypt.so", bytes("arm64-conscrypt"));
            multiAbi.put("lib/arm64-v8a/libcronet.so", bytes("arm64-cronet"));
            multiAbi.put("lib/x86_64/libconscrypt.so", x64Conscrypt);
            multiAbi.put("lib/x86_64/libcronet.so", x64Cronet);
            writeZip(gmsApk, multiAbi);
            long initialMtimeMs = 1_700_000_100_000L;
            Files.setLastModifiedTime(gmsApk, FileTime.fromMillis(initialMtimeMs));

            ApkPackage gmsPkg = allocateApkPackage(unsafe, gmsApk.toString());
            int extracted = (Integer) invoke(prepareNativeLibraries, null, gmsPkg);
            Path expectedX64Dir = gmsDir.resolve("lib/x86_64");
            check(extracted == 2, "Expected 2 x86_64 libs extracted, got " + extracted);
            check(expectedX64Dir.toString().equals(gmsPkg.nativeLibraryDir()),
                    "Expected nativeLibraryDir=" + expectedX64Dir + ", got " + gmsPkg.nativeLibraryDir());
            check("x86_64".equals(primaryCpuAbiField.get(gmsPkg)),
                    "Expected primaryCpuAbi=x86_64, got " + primaryCpuAbiField.get(gmsPkg));
            check(Arrays.equals(x64Conscrypt, Files.readAllBytes(expectedX64Dir.resolve("libconscrypt.so"))),
                    "Extracted libconscrypt.so must match x86_64 entry");
            check(Arrays.equals(x64Cronet, Files.readAllBytes(expectedX64Dir.resolve("libcronet.so"))),
                    "Extracted libcronet.so must match x86_64 entry");

            // 2. Idempotent second run returns 0 and preserves files.
            int secondRun = (Integer) invoke(prepareNativeLibraries, null, gmsPkg);
            check(secondRun == 0, "Expected idempotent second run to extract 0 libs, got " + secondRun);
            check(Arrays.equals(x64Cronet, Files.readAllBytes(expectedX64Dir.resolve("libcronet.so"))),
                    "Idempotent run must keep libcronet.so intact");

            // 3a. Missing .so recovery.
            Files.delete(expectedX64Dir.resolve("libcronet.so"));
            int afterDelete = (Integer) invoke(prepareNativeLibraries, null, gmsPkg);
            check(afterDelete == 2, "Deleting an extracted lib must trigger re-extraction");
            check(Arrays.equals(x64Cronet, Files.readAllBytes(expectedX64Dir.resolve("libcronet.so"))),
                    "Re-extraction must restore deleted libcronet.so");

            // 3b. Truncated .so recovery.
            Files.write(expectedX64Dir.resolve("libconscrypt.so"), bytes("short"));
            int afterTruncate = (Integer) invoke(prepareNativeLibraries, null, gmsPkg);
            check(afterTruncate == 2, "Truncating an extracted lib must trigger re-extraction");
            check(Arrays.equals(x64Conscrypt, Files.readAllBytes(expectedX64Dir.resolve("libconscrypt.so"))),
                    "Re-extraction must restore truncated libconscrypt.so");

            // 4. Replaced APK with older mtime + same-size modified content + stale .so cleanup.
            byte[] x64ConscryptV2 = bytes("x64-conscrypt-payload-v2");
            check(x64ConscryptV2.length == x64Conscrypt.length, "Test fixture requires equal size");
            byte[] x64CronetV2 = bytes("x64-cronet-v2");
            Map<String, byte[]> upgraded = new LinkedHashMap<>();
            upgraded.put("classes.dex", bytes("dex2"));
            upgraded.put("lib/x86_64/libconscrypt.so", x64ConscryptV2);
            upgraded.put("lib/x86_64/libcronet_v2.so", x64CronetV2);
            writeZip(gmsApk, upgraded);
            long olderMtimeMs = initialMtimeMs - 60_000L;
            Files.setLastModifiedTime(gmsApk, FileTime.fromMillis(olderMtimeMs));

            int afterReplace = (Integer) invoke(prepareNativeLibraries, null, gmsPkg);
            check(afterReplace == 2, "Replaced APK with older mtime must re-extract");
            check(Arrays.equals(x64ConscryptV2, Files.readAllBytes(expectedX64Dir.resolve("libconscrypt.so"))),
                    "Same-size replaced libconscrypt.so must be updated");
            check(Arrays.equals(x64CronetV2, Files.readAllBytes(expectedX64Dir.resolve("libcronet_v2.so"))),
                    "New libcronet_v2.so must be extracted");
            check(!Files.exists(expectedX64Dir.resolve("libcronet.so")),
                    "Stale libcronet.so must be removed after APK update");

            // 5. ARM64-only APK extracts into lib/arm64 and sets primaryCpuAbi=arm64-v8a.
            Path armDir = root.resolve("priv-app/com.android.vending");
            Path armApk = armDir.resolve("base.apk");
            byte[] armPayload = bytes("arm64-only-lib");
            writeZip(armApk, Map.of("lib/arm64-v8a/libarmhelper.so", armPayload));
            ApkPackage armPkg = allocateApkPackage(unsafe, armApk.toString());
            int armCount = (Integer) invoke(prepareNativeLibraries, null, armPkg);
            Path expectedArmDir = armDir.resolve("lib/arm64");
            check(armCount == 1, "Expected 1 arm64 lib extracted, got " + armCount);
            check(expectedArmDir.toString().equals(armPkg.nativeLibraryDir()),
                    "Expected arm64 nativeLibraryDir=" + expectedArmDir + ", got " + armPkg.nativeLibraryDir());
            check("arm64-v8a".equals(primaryCpuAbiField.get(armPkg)),
                    "Expected primaryCpuAbi=arm64-v8a");
            check(Arrays.equals(armPayload, Files.readAllBytes(expectedArmDir.resolve("libarmhelper.so"))),
                    "ARM64 library content mismatch");

            // 6. No-native-code APK sets fallback x86_64 dir, null ABI, and creates no lib dir.
            Path gsfDir = root.resolve("priv-app/com.google.android.gsf");
            Path gsfApk = gsfDir.resolve("base.apk");
            writeZip(gsfApk, Map.of("classes.dex", bytes("dex")));
            ApkPackage gsfPkg = allocateApkPackage(unsafe, gsfApk.toString());
            int gsfCount = (Integer) invoke(prepareNativeLibraries, null, gsfPkg);
            check(gsfCount == 0, "Expected 0 libs for no-native APK");
            check(gsfDir.resolve("lib/x86_64").toString().equals(gsfPkg.nativeLibraryDir()),
                    "Expected fallback nativeLibraryDir for no-native APK");
            check(primaryCpuAbiField.get(gsfPkg) == null,
                    "Expected primaryCpuAbi=null for no-native APK");
            check(!Files.exists(gsfDir.resolve("lib")),
                    "No-native APK must not create lib directory");

            // 7. Reject non-directory target path.
            Path badDir = root.resolve("priv-app/bad/lib/x86_64");
            Files.createDirectories(badDir.getParent());
            Files.write(badDir, bytes("not-a-directory"));
            boolean rejected = false;
            try {
                invoke(syncNativeLibraries, null, gmsApk.toFile(), badDir.toFile(), "x86_64");
            } catch (IOException expected) {
                rejected = true;
            }
            check(rejected, "Non-directory native library target must throw IOException");

            System.out.println("MicrogAddonNativeLibsTest all passed (" + checks + " checks)");
        } finally {
            deleteTree(root);
        }
    }
}
