/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.content.pm;

public class Signature {
    private final String mSignature;

    public Signature(String text) {
        mSignature = text;
    }

    public String toCharsString() {
        return mSignature;
    }

    public byte[] toByteArray() {
        return mSignature != null ? mSignature.getBytes() : new byte[0];
    }
}
