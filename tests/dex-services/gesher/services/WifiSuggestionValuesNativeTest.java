/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

/** Initializes the genuine native runtime for the standalone Windows ART process. */
public final class WifiSuggestionValuesNativeTest {
    public static void main(String[] args) {
        System.loadLibrary("android_runtime");
        WifiSuggestionValuesContractTest.main(args);
    }
}
