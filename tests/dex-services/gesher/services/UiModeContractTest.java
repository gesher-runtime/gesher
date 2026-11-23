/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

/** Windows/ART entry point: generated Binder proxy, real service and lifecycle transactions. */
public final class UiModeContractTest {
    public static void main(String[] args) throws Exception {
        int checks = com.gesher.server.UiModeFixture.run(true);
        System.out.println("UiModeContractTest all passed: " + checks + " checks; binder=true");
    }
}
