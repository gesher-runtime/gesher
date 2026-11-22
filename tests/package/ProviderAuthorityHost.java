/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class ProviderAuthorityHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        boolean baseline = args.length > 1 && args[1].equals("baseline");
        if (!baseline) {
            gesher.services.ProviderInitOrderTest.declarationContracts();
            gesher.services.ProviderAuthorityIndexTest.contracts();
        }
        gesher.services.ProviderAuthorityIndexTest.benchmark(!baseline);
        System.out.println("Actual PMS/ContentProviders/ApkPackage/framework metadata; constructed manifests and host JNI boundaries. No Windows/ART or installer execution claim.");
    }
}
