/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.pm.Signature;
import android.os.Process;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;
import java.util.HexFormat;

/** Moves legacy key encodings out of the app's readable storage before it runs. */
final class LegacyKeyStoreArchive {
    private LegacyKeyStoreArchive() {}

    static void prepare(ApkPackage owner) throws IOException {
        // Boot storage preparation can precede assignment of the package's UID.
        if ("android".equals(owner.packageName)
                || owner.uid() >= 0 && owner.uid() < Process.FIRST_APPLICATION_UID) return;
        File root = new File(Device.dataDir(), "system/legacy-key-archive");
        String archive = Zygote.windowsPath(root.getAbsolutePath());
        Signature[] current = owner.verifiedSignatures();
        String[] signers = current == null ? new String[0] : Arrays.stream(current)
                .map(signer -> HexFormat.of().formatHex(sha256().digest(signer.toByteArray())))
                .sorted().toArray(String[]::new);
        archive(owner, archive, owner.credentialProtectedDataDir(), "ce", signers);
        archive(owner, archive, owner.deviceProtectedDataDir(), "de", signers);
    }

    private static void archive(ApkPackage owner, String root, String directory,
            String area, String[] signers) throws IOException {
        String path = Zygote.windowsPath(directory);
        String identity = owner.packageName + '\0' + owner.firstInstallTime() + '\0'
                + area + '\0' + path + '\0' + String.join("\n", signers);
        String binding = HexFormat.of().formatHex(sha256().digest(identity.getBytes(StandardCharsets.UTF_8)));
        int error = Zygote.nativeArchiveLegacyKeys(path, root, binding);
        if (error != 0) {
            throw new IOException("Cannot protect the legacy AndroidKeyStore for " + owner.packageName
                    + " (" + area + ", Win32 " + Integer.toUnsignedString(error) + ")");
        }
    }

    private static MessageDigest sha256() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
    }

}
