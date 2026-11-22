/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class SchedulerWorkHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        if (args[1].equals("admission-work")) {
            gesher.services.JobSchedulerAdmissionTest.measure(Integer.parseInt(args[2]));
            return;
        }
        if (!args[1].equals("constraints") && !args[1].equals("admission")) {
            gesher.services.SchedulerWorkContractTest.contracts(args[1], false);
        }
        if (args[1].equals("all") || args[1].equals("admission")) {
            gesher.services.JobSchedulerAdmissionTest.contracts(false);
        }
        if (args[1].equals("all") || args[1].equals("constraints")) {
            gesher.services.JobSchedulerConstraintContractTest.contracts();
            System.out.println("JobSchedulerConstraintContractTest all passed: "
                    + gesher.services.JobSchedulerConstraintContractTest.checks + " checks");
        }
        System.out.println("Scheduler work host all passed: " + gesher.services.SchedulerWorkContractTest.checks + " checks");
        System.out.println("Real framework/public Java APIs and production services; constructed package/process metadata, manual run dispatch, native identity/allocation boundaries and write-size-only Parcel. No host ART, Binder transport, Windows timer or notification publication claim.");
    }
}
