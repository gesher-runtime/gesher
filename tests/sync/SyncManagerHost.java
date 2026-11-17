/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class SyncManagerHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        // Account-removal races invalidate real framework caches in the host's
        // official process-local nonce mode; Android shared memory is not mapped.
        android.app.PropertyInvalidatedCache.setTestMode(true);
        gesher.services.SyncManagerContractTest.contracts(args[1], false);
        System.out.println("Sync manager host passed: " + gesher.services.SyncManagerContractTest.checks + " checks");
    }
}
