/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

/** Actual public ComponentCaller/ActivityClient and AIDL, without starting an app Activity. */
public final class ComponentCallerContractTest {
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        int checks = com.gesher.server.ComponentCallerFixture.run(true);
        System.out.println("ComponentCallerContractTest all passed: " + checks + " checks");
    }
}
