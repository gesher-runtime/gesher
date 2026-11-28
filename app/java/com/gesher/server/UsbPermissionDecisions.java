/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.pm.Signature;
import android.hardware.usb.UsbDevice;
import android.util.AtomicFile;

import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;
import java.util.Properties;

/** Persistent Android USB decisions. These never authorize raw block access. */
final class UsbPermissionDecisions {
    private static final int MAX_ENTRIES = 4096;
    private static final int MAX_BYTES = 1024 * 1024;
    private final AtomicFile file;
    private Properties decisions = new Properties();

    UsbPermissionDecisions(File path) {
        file = new AtomicFile(path);
        try (var input = file.openRead()) {
            byte[] bytes = input.readNBytes(MAX_BYTES + 1);
            if (bytes.length > MAX_BYTES) throw new IOException("USB permission state is too large");
            decisions.load(new java.io.ByteArrayInputStream(bytes));
            if (decisions.size() > MAX_ENTRIES) throw new IOException("Too many USB permission decisions");
            for (String key : decisions.stringPropertyNames()) {
                if (!key.matches("[0-9a-f]{64}") ||
                        !java.util.Set.of("true", "false").contains(decisions.getProperty(key))) {
                    throw new IOException("Invalid USB permission state");
                }
            }
        } catch (FileNotFoundException absent) {
            // No decision has been made yet.
        } catch (IOException | IllegalArgumentException failure) {
            decisions.clear();
            SystemServer.log("USB persistent permissions unavailable: " + failure.getClass().getSimpleName());
        }
    }

    private static String hash(String... values) {
        try {
            var bytes = new ByteArrayOutputStream();
            var encoded = new DataOutputStream(bytes);
            for (String value : values) {
                if (value == null) { encoded.writeInt(-1); continue; }
                byte[] text = value.getBytes(StandardCharsets.UTF_8);
                encoded.writeInt(text.length);
                encoded.write(text);
            }
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(bytes.toByteArray());
            StringBuilder result = new StringBuilder(64);
            for (byte value : digest) {
                result.append(Character.forDigit((value >>> 4) & 15, 16));
                result.append(Character.forDigit(value & 15, 16));
            }
            return result.toString();
        } catch (IOException | NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
    }

    static String deviceKey(UsbDevice device, String serial) {
        // AOSP DeviceFilter identity. The attachment address is deliberately
        // absent so a persistent decision survives unplugging the same device.
        return hash("device", Integer.toString(device.getVendorId()), Integer.toString(device.getProductId()),
                Integer.toString(device.getDeviceClass()), Integer.toString(device.getDeviceSubclass()),
                Integer.toString(device.getDeviceProtocol()), device.getManufacturerName(),
                device.getProductName(), serial);
    }

    private static String entryKey(String device, ApkPackage installation) {
        String owner = installation.enabledStateOwner();
        if (owner == null || owner.equals("00000000-0000-0000-0000-000000000000")) return null;
        try {
            if (!java.util.UUID.fromString(owner).toString().equals(owner)) return null;
        } catch (IllegalArgumentException invalid) {
            return null;
        }
        Signature[] signatures = installation.verifiedSignatures();
        if (signatures == null || signatures.length == 0) return null;
        String[] identity = Arrays.stream(signatures).map(Signature::toCharsString).sorted().toArray(String[]::new);
        // A valid update retains its owner; uninstall/reinstall does not, even
        // when package, UID and signer are reused. The domain also makes old
        // owner-less rows inert. Retain those bounded records without granting.
        return hash("installation-v2", device, installation.packageName,
                Integer.toString(installation.uid()), owner, hash(identity));
    }

    synchronized Boolean get(String device, ApkPackage installation) {
        String key = entryKey(device, installation);
        String value = key == null ? null : decisions.getProperty(key);
        return value == null ? null : Boolean.valueOf(value);
    }

    synchronized void set(String device, java.util.List<ApkPackage> installations, boolean granted) {
        Properties changed = new Properties();
        changed.putAll(decisions);
        for (ApkPackage installation : installations) {
            String key = entryKey(device, installation);
            if (key == null) throw new SecurityException("Cannot persist USB permission without verified installation identity");
            changed.setProperty(key, Boolean.toString(granted));
        }
        if (changed.equals(decisions)) return;
        if (changed.size() > MAX_ENTRIES) throw new IllegalStateException("USB permission limit reached");
        var bytes = new ByteArrayOutputStream();
        try {
            changed.store(bytes, "Gesher Android USB permission decisions");
            if (bytes.size() > MAX_BYTES) throw new IOException("USB permission state is too large");
            java.io.FileOutputStream output = null;
            try {
                output = file.startWrite();
                output.write(bytes.toByteArray());
                output.getFD().sync();
                file.finishWrite(output);
            } catch (IOException | RuntimeException failure) {
                if (output != null) file.failWrite(output);
                throw failure;
            }
            // AtomicFile.finishWrite has a void result: some Android versions
            // log a failed replacement instead of throwing it. A successful
            // setter must represent the decision that was actually committed.
            try (var input = file.openRead()) {
                if (!Arrays.equals(bytes.toByteArray(), input.readNBytes(MAX_BYTES + 1))) {
                    throw new IOException("USB permission replacement did not commit");
                }
            }
            decisions = changed;
        } catch (IOException failure) {
            throw new IllegalStateException("Cannot persist USB permission decision", failure);
        }
    }
}
