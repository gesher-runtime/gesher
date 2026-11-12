/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class GmsSetupHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        gesher.services.GmsSetupContractTest.contracts();
        System.out.println("Real service code with synthetic process/package records and existing native host boundaries; no Windows shutdown, Binder transport or DPAPI claim.");
    }
}
