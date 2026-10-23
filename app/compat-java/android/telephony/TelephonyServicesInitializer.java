/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.telephony;

/**
 * No-op stand-in for the telephony module's service initializer, which framework.jar's
 * TelephonyFrameworkInitializer calls from SystemServiceRegistry's static initializer.
 * framework.jar does not contain it; this registers none of the module's services.
 */
public final class TelephonyServicesInitializer {
    private TelephonyServicesInitializer() {
    }

    public static void registerServiceWrappers() {
    }
}
