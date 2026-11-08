/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.content.res.AssetManager;

import java.io.File;
import java.io.RandomAccessFile;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;

/** Actual Installer rejection paths, preserving the previous PM record and app data. */
public final class InstallerSigningTest {
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
    }

    private static Object construct(Class<?> type, Class<?>[] parameters, Object... values) throws Exception {
        Constructor<?> constructor = type.getDeclaredConstructor(parameters);
        constructor.setAccessible(true); return constructor.newInstance(values);
    }

    private static Object parse(File file) throws Exception {
        return Class.forName("com.gesher.server.ApkPackage").getMethod("parse", String.class)
                .invoke(null, file.getPath());
    }

    private static void reject(Object previous, File incoming, String code) throws Exception {
        Class<?> apkClass = previous.getClass();
        String pkg = (String) apkClass.getField("packageName").get(previous);
        Class<?> pmClass = Class.forName("com.gesher.server.PackageManagerService");
        Class<?> atmClass = Class.forName("com.gesher.server.ActivityTaskManagerService");
        Class<?> installerClass = Class.forName("com.gesher.server.Installer");
        Object pm = construct(pmClass, new Class<?>[] {java.util.Collection.class}, Collections.singletonList(previous));
        Object atm = construct(atmClass, new Class<?>[] {pmClass}, pm);
        Object installer = construct(installerClass, new Class<?>[] {pmClass, atmClass, Map.class},
                pm, atm, new LinkedHashMap<>());
        String dataDir = (String) apkClass.getMethod("dataDir").invoke(previous);
        File marker = new File(dataDir, "identity-owned-data.txt");
        Files.createDirectories(marker.toPath().getParent());
        byte[] data = "non-personal existing package data".getBytes(StandardCharsets.UTF_8);
        Files.write(marker.toPath(), data);
        Method install = installerClass.getDeclaredMethod("install", String.class); install.setAccessible(true);
        try {
            install.invoke(installer, incoming.getPath());
            throw new AssertionError("Installer accepted " + code);
        } catch (InvocationTargetException rejected) {
            Throwable cause = rejected.getCause();
            require(cause instanceof SecurityException, "Install failed for the wrong reason: " + cause);
            require(cause.getMessage().contains(code), "Unexpected install rejection: " + cause.getMessage());
        }
        require(pmClass.getMethod("get", String.class).invoke(pm, pkg) == previous,
                "Rejected update replaced the previous PM object");
        require(java.util.Arrays.equals(data, Files.readAllBytes(marker.toPath())),
                "Rejected update changed the package's existing data");
        System.out.println("InstallerSigningTest: " + code + " preserves installed code identity and data");
    }

    private static void forwardRotation(File data, File oldApk, File rotatedApk) throws Exception {
        File oldCode = new File(data, "forward-code-1"); Files.createDirectories(oldCode.toPath());
        File base = new File(oldCode, "base.apk"); Files.copy(oldApk.toPath(), base.toPath());
        Object previous = parse(base);
        Class<?> apkClass = previous.getClass();
        String pkg = (String) apkClass.getField("packageName").get(previous);
        Class<?> pmClass = Class.forName("com.gesher.server.PackageManagerService");
        Class<?> atmClass = Class.forName("com.gesher.server.ActivityTaskManagerService");
        Class<?> amClass = Class.forName("com.gesher.server.ActivityManagerService");
        Class<?> notifications = Class.forName("com.gesher.server.NotificationManagerService");
        Class<?> installerClass = Class.forName("com.gesher.server.Installer");
        Class<?> entryClass = Class.forName(installerClass.getName() + "$Entry");
        Object pm = construct(pmClass, new Class<?>[] {java.util.Collection.class}, Collections.singletonList(previous));
        Object atm = construct(atmClass, new Class<?>[] {pmClass}, pm);
        construct(amClass, new Class<?>[] {atmClass, pmClass, notifications}, atm, pm,
                construct(notifications, new Class<?>[0]));
        Map<String, Object> entries = new LinkedHashMap<>();
        entries.put(pkg, construct(entryClass, new Class<?>[] {String.class, String.class, long.class, long.class},
                pkg, oldCode.getPath(), 12345L, 12345L));
        Object installer = construct(installerClass, new Class<?>[] {pmClass, atmClass, Map.class}, pm, atm, entries);
        File marker = new File((String) apkClass.getMethod("dataDir").invoke(previous), "rotation-data.txt");
        Files.createDirectories(marker.toPath().getParent());
        byte[] content = "non-personal data retained after verified rotation".getBytes(StandardCharsets.UTF_8);
        Files.write(marker.toPath(), content);
        int oldUid = (Integer) apkClass.getMethod("uid").invoke(previous);
        Method install = installerClass.getDeclaredMethod("install", String.class); install.setAccessible(true);
        String[] result = (String[]) install.invoke(installer, rotatedApk.getPath());
        require("updated".equals(result[1]), "Verified rotation did not take the install update path");
        Object updated = pmClass.getMethod("get", String.class).invoke(pm, pkg);
        require(updated != previous && oldUid == (Integer) apkClass.getMethod("uid").invoke(updated),
                "Forward rotation did not publish a new record with the stable app UID");
        require(java.util.Arrays.equals(content, Files.readAllBytes(marker.toPath())),
                "Forward rotation discarded the predecessor's data");
        require((Long) apkClass.getMethod("firstInstallTime").invoke(updated) == 12345L,
                "Forward rotation changed the first install time");
        require((Boolean) apkClass.getMethod("updateContinuityFrom", apkClass).invoke(updated, previous),
                "Copied installed code lost its verified forward lineage");
        System.out.println("InstallerSigningTest: verified forward rotation installs with stable UID and retained data");
    }

    public static void main(String[] args) throws Exception {
        require(args.length == 1, "Pass the signing fixtures directory");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        File data = Files.createTempDirectory("gesher-installer-identity-").toFile();
        android.system.Os.setenv("ANDROID_DATA", data.getAbsolutePath(), true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true);
        File fixtures = new File(args[0]);
        File v2 = new File(fixtures, "v2only.apk");
        File unsigned = new File(fixtures, "unsigned.apk");
        File rotated = new File(fixtures, "rotated.apk");
        // A predecessor key is not allowed to replace its descendant without a new
        // valid forward proof. The two fixtures have different current signer keys.
        reject(parse(rotated), v2, "INSTALL_FAILED_UPDATE_INCOMPATIBLE");
        reject(parse(v2), unsigned, "INSTALL_FAILED_UPDATE_INCOMPATIBLE");

        File changed = new File(data, "tampered.apk");
        Files.copy(v2.toPath(), changed.toPath(), StandardCopyOption.REPLACE_EXISTING);
        try (RandomAccessFile file = new RandomAccessFile(changed, "rw")) {
            require(file.readInt() == 0x504b0304, "Unexpected fixture ZIP prefix");
            // Change ZIP local-header time only: manifest still parses and the original
            // signing block/certificate remains, but the signed file content differs.
            file.seek(10); int value = file.readUnsignedByte();
            file.seek(10); file.writeByte(value ^ 1);
        }
        reject(parse(v2), changed, "INSTALL_FAILED_BAD_SIGNATURE");

        File splitDirectory = new File(data, "split-upload");
        Files.createDirectories(splitDirectory.toPath());
        File base = new File(splitDirectory, "base.apk");
        Files.copy(v2.toPath(), base.toPath());
        Files.copy(rotated.toPath(), new File(splitDirectory, "split_other.apk").toPath());
        reject(parse(v2), base, "INSTALL_FAILED_BAD_SPLIT_SIGNATURE");
        File capabilities = new File(fixtures.getParentFile(), "identity-fixtures");
        if (capabilities.isDirectory()) {
            reject(parse(new File(capabilities, "old.apk")), new File(capabilities, "rotated-no-data.apk"),
                    "INSTALL_FAILED_UPDATE_INCOMPATIBLE");
        }
        forwardRotation(data, v2, rotated);
        System.out.println("InstallerSigningTest all passed");
    }
}
