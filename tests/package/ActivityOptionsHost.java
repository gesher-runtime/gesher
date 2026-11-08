/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/** Host native-boundary adapters only; all resolver and framework Java is production code. */
public final class ActivityOptionsHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        gesher.services.ActivityOptionsContractTest.frameworkContracts();
    }
}
