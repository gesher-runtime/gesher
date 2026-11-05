/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.content.res.AssetManager;
import java.io.File;
import java.io.RandomAccessFile;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;

/** Full-verifier rotation trust: data retention, independent AUTH retention and rollback. */
public final class SigningRotationTest {
    private static void require(boolean condition, String reason) {
        if (!condition) throw new AssertionError(reason);
    }

    public static void main(String[] args) throws Exception {
        require(args.length == 2, "Pass signing fixtures and identity-fixtures directories");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        File data = Files.createTempDirectory("gesher-signing-rotation-").toFile();
        android.system.Os.setenv("ANDROID_DATA", data.getAbsolutePath(), true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true);
        Class<?> apk = Class.forName("com.gesher.server.ApkPackage");
        Method parse = apk.getMethod("parse", String.class);
        Method update = apk.getMethod("updateContinuityFrom", apk);
        Method strict = apk.getMethod("signaturesMatch", apk);
        Method current = apk.getMethod("verifiedSignerSha256");
        Method auth = apk.getMethod("verifiedAuthSignerSha256");
        Object old = parse.invoke(null, new File(args[0], "v2only.apk").getPath());
        Object rotated = parse.invoke(null, new File(args[0], "rotated.apk").getPath());
        require((Boolean) update.invoke(rotated, old), "Verified forward rotation could not retain installed data");
        require(!(Boolean) update.invoke(old, rotated), "Predecessor APK could roll back a rotated signing identity");
        require(!(Boolean) strict.invoke(rotated, old), "Cross-package current-signer equality accepted a historical key");
        require(!current.invoke(rotated).equals(current.invoke(old)), "Current fingerprint silently became an ancestor");
        require(auth.invoke(rotated).equals(current.invoke(old)), "AUTH-retained ancestor identity was lost");
        require(current.invoke(old).equals(auth.invoke(old)), "Unrotated signer AUTH identity changed");
        System.out.println("SigningRotationTest: real forward lineage accepted, rollback denied, current-key matching strict");

        Object capabilityOld = parse.invoke(null, new File(args[1], "old.apk").getPath());
        Object noData = parse.invoke(null, new File(args[1], "rotated-no-data.apk").getPath());
        Object noAuth = parse.invoke(null, new File(args[1], "rotated-no-auth.apk").getPath());
        require(current.invoke(noData) != null && current.invoke(noAuth) != null,
                "Capability fixtures did not pass full APK verification");
        require((Boolean) strict.invoke(noData, noAuth), "Capability fixtures do not share their current key");
        require(!(Boolean) update.invoke(noData, capabilityOld), "Missing INSTALLED_DATA capability was ignored");
        require((Boolean) update.invoke(noAuth, capabilityOld), "INSTALLED_DATA was incorrectly tied to AUTH");
        require(auth.invoke(noData).equals(current.invoke(capabilityOld)), "Independent AUTH retention was ignored");
        require(auth.invoke(noAuth).equals(current.invoke(noAuth)), "Revoked AUTH inherited an ancestor's authority");
        require(!auth.invoke(noAuth).equals(current.invoke(capabilityOld)), "AUTH revocation did not change identity");
        require(!(Boolean) update.invoke(noAuth, old), "Unrelated certificate was accepted as a predecessor");
        System.out.println("SigningRotationTest: INSTALLED_DATA and AUTH capabilities enforced independently");

        File changed = new File(data, "tampered-rotation.apk");
        Files.copy(new File(args[0], "rotated.apk").toPath(), changed.toPath(), StandardCopyOption.REPLACE_EXISTING);
        try (RandomAccessFile file = new RandomAccessFile(changed, "rw")) {
            require(file.readInt() == 0x504b0304, "Unexpected fixture ZIP prefix");
            file.seek(10); int timestamp = file.readUnsignedByte(); file.seek(10); file.writeByte(timestamp ^ 1);
        }
        Object unverified = parse.invoke(null, changed.getPath());
        require(!(Boolean) update.invoke(unverified, old), "Copied rotation block authorized changed APK contents");
        require(auth.invoke(unverified) == null, "Unverified rotation proof supplied an AUTH identity");
        System.out.println("SigningRotationTest: content-modified APK cannot use copied lineage capabilities");
        System.out.println("SigningRotationTest all passed");
    }
}
