/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

public final class PackageEnabledLifecycleTest {
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        int checks = com.gesher.server.PackageEnabledLifecycleFixture.run(true, args.length == 0 ? "all" : args[0]);
        System.out.println("Package enabled lifecycle contracts passed: " + checks + " checks; actual ART and PM AIDL");
    }
}
