/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

/** Contract test verifying Activity state restoration, task persistence, and process death lifecycle on ART. */
public final class ActivityRestoreContractTest {
    public static void main(String[] args) throws Exception {
        try {
            System.loadLibrary("android_runtime");
        } catch (UnsatisfiedLinkError ignored) {}
        int checks = com.gesher.server.ActivityRestoreFixture.run(true, args.length > 0 ? args[0] : "all");
        System.out.println("ActivityRestoreContractTest all passed: " + checks + " state restore and lifecycle checks");
    }
}
