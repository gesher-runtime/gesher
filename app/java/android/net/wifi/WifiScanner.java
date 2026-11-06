/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net.wifi;

/**
 * Linkable type for Android's optional hardware scanner capability check.
 * Gesher supplies no scanner service: both named and typed Context lookups
 * return null. There is no constructible scanner or fabricated scan operation.
 */
public class WifiScanner {
    private WifiScanner() { }
}
