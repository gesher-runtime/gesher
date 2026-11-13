/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import gesher.services.NetworkBuilderContractTest;
import gesher.services.NetworkBuilderInternalContractTest;
public final class NetworkBuilderHost {
    public static void main(String[] args) {
        System.load(args[0]);
        String selected = args.length > 1 ? args[1] : "all";
        if (selected.equals("all") || selected.equals("bounds") || selected.equals("specifiers") || selected.equals("legacy") || selected.equals("ethernet")) {
            NetworkBuilderContractTest.contracts(selected, false);
        }
        if (selected.equals("all") || selected.equals("raw") || selected.equals("enterprise") || selected.equals("values")) {
            NetworkBuilderInternalContractTest.contracts(selected, false);
        }
        System.out.println("Production network values, real SDK-compiled caller; declared host initialization/logging/clock boundaries, no Parcel substitute.");
    }
}
