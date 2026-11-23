/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class ExactAlarmHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        // The ART wrapper creates the real shared cache region. The host fixture
        // uses Android's cache test mode so package replacement reaches the real
        // publication path without depending on absent cross-process nonce storage.
        android.app.PropertyInvalidatedCache.setTestMode(true);
        android.app.PropertyInvalidatedCache.disableForTestMode();
        gesher.services.ExactAlarmContractTest.contracts(false);
        System.out.println("Exact alarm host contracts passed: actual policy, services and framework clients; isolated package/process metadata and native identity boundaries. No Windows timer, native dialog or Binder transport claim.");
    }
}
