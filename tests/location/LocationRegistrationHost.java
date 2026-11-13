/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import gesher.services.LocationRegistrationContractTest;
public final class LocationRegistrationHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        LocationRegistrationContractTest.contracts(args.length > 1 ? args[1] : "all", false);
        System.out.println("Production registration and permission code; controlled Binder lifetimes and mock-provider metadata; no Windows hardware/transport claim.");
    }
}
