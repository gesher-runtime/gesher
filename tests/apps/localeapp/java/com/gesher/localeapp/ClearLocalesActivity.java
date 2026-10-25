/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.localeapp;

/** Clears the app's locales (an empty list: the system's again). */
public final class ClearLocalesActivity extends SetLocalesActivity {
    @Override
    String locales() {
        return "";
    }
}
