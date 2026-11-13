/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class SyncManagerHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        gesher.services.SyncManagerContractTest.contracts(args[1], false);
        System.out.println("Sync manager host passed: " + gesher.services.SyncManagerContractTest.checks + " checks");
    }
}
