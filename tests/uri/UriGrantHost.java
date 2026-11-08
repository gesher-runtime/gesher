/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/** Only native initialization/caller identity is adapted; production Java owns all policy. */
public final class UriGrantHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        gesher.services.UriGrantContractTest.contracts(args.length > 1 ? args[1] : null, false);
        System.out.println("Actual service/provider/permission classes; controlled Binder identity, not Binder transport or ART.");
    }
}
