/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net;

public class Uri {
    private String mUri;

    public static Uri parse(String uri) {
        Uri u = new Uri();
        u.mUri = uri;
        return u;
    }

    @Override
    public String toString() {
        return mUri != null ? mUri : "";
    }
}
