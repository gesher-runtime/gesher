/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.pm.Signature;
import android.hardware.usb.UsbDevice;
import android.util.AtomicFile;

import java.io.File;
import java.nio.file.Files;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;

/** Production decision store over real temporary files and controlled Android boundaries. */
public final class UsbPermissionsHost {
    private static int checks;
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        ++checks;
    }
    private static ApkPackage installed(String name, int uid, String... signers) {
        Signature[] signatures = java.util.Arrays.stream(signers).map(Signature::new).toArray(Signature[]::new);
        return new ApkPackage(name, uid, signatures);
    }
    private static void throwsType(Class<? extends Throwable> type, Runnable operation, String message) {
        try { operation.run(); }
        catch (Throwable failure) {
            require(type.isInstance(failure), message + ": " + failure);
            return;
        }
        throw new AssertionError(message + ": no failure");
    }

    public static void main(String[] args) throws Exception {
        var directory = Files.createTempDirectory("gesher-usb-decisions-");
        try {
            File file = directory.resolve("system/usb-permissions").toFile();
            UsbDevice first = new UsbDevice("/dev/bus/usb/001/001", 0x1234, 0x5678, "reader", "media");
            UsbDevice reattached = new UsbDevice("/dev/bus/usb/003/017", 0x1234, 0x5678, "reader", "media");
            String device = UsbPermissionDecisions.deviceKey(first, "serial-A");
            require(device.equals(UsbPermissionDecisions.deviceKey(reattached, "serial-A")),
                    "new attachment address changed stable device identity");
            require(!device.equals(UsbPermissionDecisions.deviceKey(first, "serial-B")),
                    "another reader serial inherited the decision");
            require(!device.equals(UsbPermissionDecisions.deviceKey(first, null)),
                    "missing serial inherited a serial-bound decision");
            ApkPackage app = installed("com.example.reader", 10123, "aabb", "1122");
            UsbPermissionDecisions store = new UsbPermissionDecisions(file);
            require(store.get(device, app) == null, "an undecided installation was granted or denied implicitly");
            store.set(device, List.of(app), true);
            require(Boolean.TRUE.equals(store.get(device, app)), "successful grant was not visible");
            store = new UsbPermissionDecisions(file);
            require(Boolean.TRUE.equals(store.get(device, app)), "grant did not survive restart");
            require(Boolean.TRUE.equals(store.get(device, installed(app.packageName, app.uid(), "1122", "aabb"))),
                    "signer enumeration order changed the verified signer set");
            require(store.get(device, installed(app.packageName, app.uid(), "aabb")) == null,
                    "a different signer set inherited permission");
            require(store.get(device, installed(app.packageName, app.uid(), "different")) == null,
                    "a different signer inherited permission");
            require(store.get(device, installed(app.packageName, app.uid() + 1, "aabb", "1122")) == null,
                    "a reused package name with another UID inherited permission");
            require(store.get(device, installed("com.example.unrelated", app.uid(), "aabb", "1122")) == null,
                    "an unrelated package sharing a UID inherited permission");
            store.set(device, List.of(app), false);
            require(Boolean.FALSE.equals(new UsbPermissionDecisions(file).get(device, app)),
                    "explicit denial did not survive restart");
            UsbPermissionDecisions selected = store;
            AtomicFile.failReplacement = true;
            throwsType(IllegalStateException.class, () -> selected.set(device, List.of(app), true),
                    "a logged-only AtomicFile replacement failure reported success");
            require(Boolean.FALSE.equals(selected.get(device, app)) &&
                    Boolean.FALSE.equals(new UsbPermissionDecisions(file).get(device, app)),
                    "failed persistence granted access in memory or after restart");
            ApkPackage unverified = new ApkPackage(app.packageName, app.uid(), null);
            require(selected.get(device, unverified) == null, "unverified APK inherited permission");
            throwsType(SecurityException.class, () -> selected.set(device, List.of(unverified), true),
                    "an unverified APK was granted persistent permission");

            AtomicReference<Throwable> concurrentFailure = new AtomicReference<>();
            Thread[] writers = new Thread[8];
            for (int i = 0; i < writers.length; ++i) {
                final int number = i;
                writers[i] = new Thread(() -> {
                    try { selected.set(device, List.of(installed("com.example.concurrent" + number,
                            10200 + number, "certificate")), true); }
                    catch (Throwable failure) { concurrentFailure.compareAndSet(null, failure); }
                });
                writers[i].start();
            }
            for (Thread writer : writers) writer.join();
            require(concurrentFailure.get() == null, "concurrent persistence failed: " + concurrentFailure.get());
            UsbPermissionDecisions restarted = new UsbPermissionDecisions(file);
            for (int i = 0; i < writers.length; ++i) {
                require(Boolean.TRUE.equals(restarted.get(device,
                        installed("com.example.concurrent" + i, 10200 + i, "certificate"))),
                        "concurrent decision was lost");
            }
            Files.writeString(file.toPath(), "invalid-key=true\n");
            require(new UsbPermissionDecisions(file).get(device, app) == null,
                    "malformed state provided a persistent grant");
            Files.write(file.toPath(), new byte[1024 * 1024 + 1]);
            require(new UsbPermissionDecisions(file).get(device, app) == null,
                    "oversized state provided a persistent grant");
            System.out.println("USB persistent permission contracts passed: " + checks + " checks");
        } finally {
            try (var paths = Files.walk(directory)) {
                for (var path : paths.sorted(java.util.Comparator.reverseOrder()).toList()) Files.deleteIfExists(path);
            }
        }
    }
}
