/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class IntentCandidateHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        boolean indexed = args.length == 1;
        gesher.services.IntentCandidateIndexTest.contracts(indexed);
        gesher.services.IntentCandidateIndexTest.benchmark(indexed);
        System.out.println("Actual PMS/ApkPackage/Android intent matching and metadata; constructed manifests and host JNI boundaries. No Windows/ART or installer execution claim.");
    }
}
