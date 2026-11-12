/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class SchedulerWorkHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        gesher.services.SchedulerWorkContractTest.contracts(args[1], false);
        System.out.println("Scheduler work host all passed: " + gesher.services.SchedulerWorkContractTest.checks + " checks");
        System.out.println("Real framework/public Java APIs and production services; constructed package/process metadata, manual run dispatch, native identity/allocation boundaries and write-size-only Parcel. No host ART, Binder transport, Windows timer or notification publication claim.");
    }
}
