/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.PackageParser.PackageParserException;
import android.system.Os;
import com.android.internal.pm.parsing.pkg.ParsedPackage;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.Collection;
import java.util.List;
import java.util.Map;

/** Real PackageParser2/APK/native catalog and Installer rejection path under shipped ART.
 * The old disk generation is explicitly fixture setup; the attempted update is
 * the unmodified production Installer.install entry point, not a simulated transaction. */
public final class NativeLibraryInstallTest {
    private static int checks;
    private static void check(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        ++checks;
        System.out.println("PASS native install contract: " + reason);
    }
    private static void copy(Path source, Path target) throws Exception {
        Files.createDirectories(target.getParent());
        try (FileInputStream input = new FileInputStream(source.toFile());
             FileOutputStream output = new FileOutputStream(target.toFile())) {
            byte[] bytes = new byte[16384];
            for (int count; (count = input.read(bytes)) >= 0;) output.write(bytes, 0, count);
        }
    }
    private static Throwable cause(InvocationTargetException error) { return error.getCause(); }
    private static void missing(Method operation, Object target, Object... arguments) throws Exception {
        try {
            operation.invoke(target, arguments);
        } catch (InvocationTargetException failure) {
            Throwable error = cause(failure);
            check(error instanceof PackageParserException
                    && ((PackageParserException) error).error == PackageManager.INSTALL_FAILED_MISSING_SHARED_LIBRARY,
                    "required native absence returns INSTALL_FAILED_MISSING_SHARED_LIBRARY");
            return;
        }
        throw new AssertionError("Missing required provider was accepted");
    }
    public static void main(String[] args) throws Exception {
        Class.forName("com.gesher.app.Main").getMethod("bootstrap", String.class,
                String.class, String.class).invoke(null, "native installation", args[0], args[1]);
        Path fixtures = Path.of(args[2]), data = Path.of(args[3]).toAbsolutePath();
        Files.createDirectories(data);
        String originalRoot = System.getenv("ANDROID_ROOT"), originalData = System.getenv("ANDROID_DATA");
        String originalUidMode = System.getenv("GESHER_MICROG_SPOOF");
        try {
            Path system = fixtures.resolve("device/system");
            copy(Path.of(args[1]), system.resolve("framework/framework-res.apk"));
            Os.setenv("ANDROID_ROOT", system.toString(), true);
            Os.setenv("ANDROID_DATA", data.toString(), true);
            Os.setenv("GESHER_MICROG_SPOOF", "1", true);
            Class<?> apkType = Class.forName("com.gesher.server.ApkPackage");
            Method parse = apkType.getMethod("parse", String.class);
            Method appInfo = apkType.getMethod("applicationInfo", long.class);
            Object legacy = parse.invoke(null, fixtures.resolve("required30.apk").toString());
            ApplicationInfo legacyInfo = (ApplicationInfo) appInfo.invoke(legacy, 0L);
            check(legacyInfo.targetSdkVersion == 30 && legacyInfo.sharedLibraryInfos == null,
                    "SDK30 retains its native dependency compatibility policy");
            Object optional = parse.invoke(null, fixtures.resolve("optional31.apk").toString());
            ApplicationInfo optionalInfo = (ApplicationInfo) appInfo.invoke(optional, 0L);
            check(optionalInfo.targetSdkVersion == 31 && optionalInfo.sharedLibraryInfos == null,
                    "SDK31 optional absent provider permits the real APK parse");
            missing(parse, null, fixtures.resolve("missing31.apk").toString());
            Object present = parse.invoke(null, fixtures.resolve("required31.apk").toString());
            ApplicationInfo presentInfo = (ApplicationInfo) appInfo.invoke(present, 0L);
            check(presentInfo.sharedLibraryInfos != null && presentInfo.sharedLibraryInfos.size() == 1
                    && presentInfo.sharedLibraryInfos.get(0).isNative()
                    && presentInfo.sharedLibraryInfos.get(0).getName().equals("libcatalog.gesher.so")
                    && presentInfo.sharedLibraryFiles == null,
                    "real native declaration reaches ApplicationInfo without becoming a dex file");

            String pkg = "com.gesher.nativepolicy";
            Path code = data.resolve("app/" + pkg + "-1");
            copy(fixtures.resolve("old.apk"), code.resolve("base.apk"));
            Method parseInstalled = apkType.getMethod("parseCodeDirectory", String.class);
            Object installed = parseInstalled.invoke(null, code.toString());
            Class<?> pmType = Class.forName("com.gesher.server.PackageManagerService");
            Object pm = pmType.getConstructor(Collection.class).newInstance(List.of(installed));
            Class<?> installerType = Class.forName("com.gesher.server.Installer");
            Path registry = data.resolve("system/packages.list");
            Files.createDirectories(registry.getParent());
            byte[] registryBytes = (pkg + "\t" + code + "\t1\t1\n").getBytes(StandardCharsets.UTF_8);
            Files.write(registry, registryBytes);
            // Decode the supported legacy entry through production registry parsing.
            Method readRegistry = installerType.getDeclaredMethod("readRegistry");
            readRegistry.setAccessible(true);
            Class<?> atmType = Class.forName("com.gesher.server.ActivityTaskManagerService");
            Constructor<?> installerConstructor = installerType.getDeclaredConstructor(pmType, atmType, Map.class);
            installerConstructor.setAccessible(true);
            Object installer = installerConstructor.newInstance(pm, null, readRegistry.invoke(null));
            Path sentinel = data.resolve("data/" + pkg + "/files/retained.txt");
            Files.createDirectories(sentinel.getParent());
            byte[] retained = "retained old app data".getBytes(StandardCharsets.UTF_8);
            Files.write(sentinel, retained);
            byte[] oldBytes = Files.readAllBytes(code.resolve("base.apk"));
            Method install = installerType.getDeclaredMethod("install", String.class);
            install.setAccessible(true);
            missing(install, installer, fixtures.resolve("missing31.apk").toString());
            check(Arrays.equals(registryBytes, Files.readAllBytes(registry))
                    && Arrays.equals(oldBytes, Files.readAllBytes(code.resolve("base.apk")))
                    && Arrays.equals(retained, Files.readAllBytes(sentinel))
                    && !Files.exists(data.resolve("app/" + pkg + "-2"))
                    && pmType.getMethod("get", String.class).invoke(pm, pkg) == installed,
                    "production failed update preserves old code, data, registry and PM identity before mutation");

            // A valid feature split can contain the only native declaration.
            // Seed the real persistent UID mode: rejecting a fresh package
            // must not leave an irreversible identity allocation behind.
            Method uidFor = Class.forName("com.gesher.server.PackageUids").getMethod("uidFor", String.class);
            int seedUid = (Integer) uidFor.invoke(null, "com.gesher.nativepolicy.seed");
            Path uidRegistry = data.resolve("system/package-uids");
            byte[] uidsBefore = Files.readAllBytes(uidRegistry);
            String splitPackage = "com.gesher.nativesplitpolicy";
            Method preflight = apkType.getDeclaredMethod("parseInstallCandidate", String.class, File[].class);
            preflight.setAccessible(true);
            Path splitInputs = fixtures.resolve("split-declarations");
            String splitBase = splitInputs.resolve("base.apk").toFile().getAbsolutePath();
            File requiredSplit = splitInputs.resolve("split_required.apk").toFile();
            missing(install, installer, fixtures.resolve("split-required31.apks").toString());
            missing(preflight, null, splitBase, new File[]{requiredSplit});
            check(Arrays.equals(uidsBefore, Files.readAllBytes(uidRegistry))
                    && Arrays.equals(registryBytes, Files.readAllBytes(registry))
                    && !Files.exists(data.resolve("app/" + splitPackage + "-1"))
                    && !Files.exists(data.resolve("data/" + splitPackage))
                    && pmType.getMethod("get", String.class).invoke(pm, splitPackage) == null,
                    "split-only required absence leaves persistent UIDs, package registry and fresh install state unchanged");
            int followingUid = (Integer) uidFor.invoke(null, "com.gesher.nativepolicy.after");
            check(followingUid == seedUid + 1,
                    "rejected split-only declaration consumes no persistent application UID");

            // The directory also holds required/present alternatives and a
            // different package's APK. Only this selected optional feature is
            // input to the real cluster parser; source paths survive cleanup.
            File optionalSplit = splitInputs.resolve("split_optional.apk").toFile();
            Object splitOptional = preflight.invoke(null, splitBase, new File[]{optionalSplit});
            ParsedPackage optionalManifest = (ParsedPackage) apkType.getField("parsed").get(splitOptional);
            ApplicationInfo splitOptionalInfo = (ApplicationInfo) appInfo.invoke(splitOptional, 0L);
            check(optionalManifest.getUsesOptionalNativeLibraries().equals(List.of("libabsent.gesher.so"))
                    && optionalManifest.getUsesNativeLibraries().isEmpty()
                    && Arrays.equals(optionalManifest.getSplitNames(), new String[]{"feature_optional"})
                    && splitOptionalInfo.sharedLibraryInfos == null,
                    "real selected optional split merges its declaration and permits the absent provider");
            check(splitBase.equals(splitOptionalInfo.sourceDir)
                    && Arrays.equals(splitOptionalInfo.splitSourceDirs,
                            new String[]{optionalSplit.getAbsolutePath()}),
                    "preflight metadata retains original selected APK paths after temporary cluster cleanup");
            Object baseOnly = parse.invoke(null, splitBase);
            ParsedPackage baseManifest = (ParsedPackage) apkType.getField("parsed").get(baseOnly);
            String[] baseSplits = baseManifest.getSplitNames();
            check((baseSplits == null || baseSplits.length == 0)
                    && baseManifest.getUsesNativeLibraries().isEmpty()
                    && baseManifest.getUsesOptionalNativeLibraries().isEmpty(),
                    "single external APK parsing still ignores all unrelated sibling APKs");
            Object splitPresent = preflight.invoke(null, splitBase,
                    new File[]{splitInputs.resolve("split_present.apk").toFile()});
            ApplicationInfo splitPresentInfo = (ApplicationInfo) appInfo.invoke(splitPresent, 0L);
            check(splitPresentInfo.sharedLibraryInfos != null
                    && splitPresentInfo.sharedLibraryInfos.size() == 1
                    && splitPresentInfo.sharedLibraryInfos.get(0).isNative()
                    && splitPresentInfo.sharedLibraryInfos.get(0).getName().equals("libcatalog.gesher.so")
                    && splitPresentInfo.sharedLibraryFiles == null,
                    "required present split declaration reaches native metadata without entering Java dex files");
            Object splitLegacy = preflight.invoke(null,
                    fixtures.resolve("split-legacy/base.apk").toString(),
                    new File[]{fixtures.resolve("split-legacy/split_native.apk").toFile()});
            ParsedPackage legacyManifest = (ParsedPackage) apkType.getField("parsed").get(splitLegacy);
            ApplicationInfo splitLegacyInfo = (ApplicationInfo) appInfo.invoke(splitLegacy, 0L);
            check(legacyManifest.getUsesNativeLibraries().contains("libabsent.gesher.so")
                    && splitLegacyInfo.targetSdkVersion == 30 && splitLegacyInfo.sharedLibraryInfos == null,
                    "SDK30 compatibility policy applies after the real feature declaration is merged");

            byte[] uidsBeforeUpdate = Files.readAllBytes(uidRegistry);
            missing(install, installer, fixtures.resolve("split-update31.apks").toString());
            Method codePaths = installerType.getDeclaredMethod("codePaths");
            codePaths.setAccessible(true);
            check(Arrays.equals(uidsBeforeUpdate, Files.readAllBytes(uidRegistry))
                    && Arrays.equals(registryBytes, Files.readAllBytes(registry))
                    && Arrays.equals(oldBytes, Files.readAllBytes(code.resolve("base.apk")))
                    && Arrays.equals(retained, Files.readAllBytes(sentinel))
                    && !Files.exists(data.resolve("app/" + pkg + "-2"))
                    && pmType.getMethod("get", String.class).invoke(pm, pkg) == installed
                    && codePaths.invoke(installer).equals(Map.of(pkg, code.toString())),
                    "split-only required update preserves old generation, data, UID registry and installer/PM state");

            Path provider = fixtures.resolve("device/system/lib64/libcatalog.gesher.so");
            Path hidden = provider.resolveSibling(provider.getFileName() + ".temporarily-absent");
            Files.move(provider, hidden);
            try {
                missing(parse, null, fixtures.resolve("required31.apk").toString());
                try {
                    appInfo.invoke(present, 0L);
                    throw new AssertionError("Stale native metadata was republished");
                } catch (InvocationTargetException expected) {
                    check(cause(expected) instanceof IllegalStateException,
                            "long-lived package metadata revalidates removed native providers");
                }
            } finally {
                Files.move(hidden, provider);
            }
            System.out.println("NativeLibraryInstallTest all passed: " + checks + " real APK/Installer checks");
        } finally {
            if (originalRoot == null) Os.unsetenv("ANDROID_ROOT"); else Os.setenv("ANDROID_ROOT", originalRoot, true);
            if (originalData == null) Os.unsetenv("ANDROID_DATA"); else Os.setenv("ANDROID_DATA", originalData, true);
            if (originalUidMode == null) Os.unsetenv("GESHER_MICROG_SPOOF");
            else Os.setenv("GESHER_MICROG_SPOOF", originalUidMode, true);
        }
    }
}
