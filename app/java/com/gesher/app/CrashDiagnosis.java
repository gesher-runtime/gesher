/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.app;

import java.util.function.BooleanSupplier;

/** Conservative hints for a host crash notice; never changes an Android API result. */
public final class CrashDiagnosis {
    private CrashDiagnosis() {}

    public static String kind(Throwable failure, BooleanSupplier bluetoothPresent) {
        String fallback = "generic";
        // Bound even malformed/cyclic cause chains supplied by application code.
        for (int i = 0; failure != null && i < 32; i++, failure = failure.getCause()) {
            String message = failure.getMessage();
            if (failure instanceof NullPointerException && message != null
                    && message.startsWith("Attempt to invoke virtual method '")
                    && message.contains(" android.bluetooth.BluetoothAdapter.")
                    && message.endsWith("' on a null object reference")) {
                try {
                    if (!bluetoothPresent.getAsBoolean()) return "bluetooth";
                } catch (Throwable unavailable) {
                    // A failed probe cannot establish that hardware is absent.
                }
            }
            if (failure instanceof SecurityException) fallback = "security";
            else if (failure instanceof LinkageError && fallback.equals("generic")) {
                fallback = "compatibility";
            }
        }
        return fallback;
    }
}
