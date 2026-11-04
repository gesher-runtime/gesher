/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.content.pm;

public final class SigningDetails {
    public static final class SignatureSchemeVersion {
        public static final int SIGNING_BLOCK_V3 = 3;
    }

    final Signature[] mSignatures;

    public SigningDetails(Signature[] signatures, int signatureSchemeVersion,
            java.util.Set<java.security.PublicKey> keys, Signature[] pastSigningCertificates) {
        mSignatures = signatures;
    }
}
