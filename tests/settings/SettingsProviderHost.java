/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class SettingsProviderHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        // These direct calls cover storage/CRUD as the owning server. Ordinary remote
        // reads and denied writes are covered by the separate Binder transport fixture.
        long identity = android.os.Binder.clearCallingIdentity();
        try { gesher.services.SettingsProviderContractTest.contracts(args[1]); }
        finally { android.os.Binder.restoreCallingIdentity(identity); }
        System.out.println("Settings provider host passed: " + gesher.services.SettingsProviderContractTest.checks + " checks");
    }
}
