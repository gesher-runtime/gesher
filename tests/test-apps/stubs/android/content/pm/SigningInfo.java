/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.content.pm;

public final class SigningInfo {
    private final SigningDetails mDetails;

    public SigningInfo(SigningDetails details) {
        mDetails = details;
    }

    public Signature[] getApkContentsSigners() {
        return mDetails.mSignatures;
    }
}
