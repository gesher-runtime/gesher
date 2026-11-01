/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.os;

public class Looper {
    private static final Looper sMainLooper = new Looper();

    public static Looper getMainLooper() {
        return sMainLooper;
    }
}
