/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class DomainVerificationHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        int checks = gesher.services.DomainVerificationContractTest.run(false);
        System.out.println("Domain verification host all passed: " + checks + " checks");
        System.out.println("Production service and actual Android manager/values; controlled native host boundaries. "
                + "Binder transport is covered by the Windows fixture.");
    }
}
