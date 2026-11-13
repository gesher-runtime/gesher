/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class SettingsProviderHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        android.os.Binder.restoreCallingIdentity(((long)10042 << 32) | 834121);
        gesher.services.SettingsProviderContractTest.contracts(args[1]);
        if (android.os.Binder.getCallingUid() != 10042) throw new AssertionError("Caller identity was not restored");
        System.out.println("Settings provider host passed: " + gesher.services.SettingsProviderContractTest.checks + " checks");
    }
}
