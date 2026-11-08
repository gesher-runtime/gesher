/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class ZygotePathHost {
    public static void main(String[] args) {
        System.load(args[0]);
        gesher.services.ZygotePathTest.contracts();
    }
}
