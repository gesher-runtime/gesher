/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

/** Actual request policy with an injected server-local radio; no prompt or hardware mutation. */
public final class BluetoothRequestIntentTest {
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        int checks = com.gesher.server.BluetoothRequestFixture.run();
        System.out.println("BluetoothRequestIntentTest all passed: " + checks);
    }
}
